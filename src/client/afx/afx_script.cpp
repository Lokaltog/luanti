// Luanti "afx" fork: the client's ambience runtime, engine side
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "afx_script.h"
#include "afx.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientobject.h"
#include "client/mod_vfs.h"
#include "chatmessage.h"
#include <map>
#include "constants.h"
#include "exceptions.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"
#include "script/scripting_afx.h"
#include "settings.h"
#include <memory>

namespace afx
{

static ScriptContext s_ctx;
static std::unique_ptr<ModVFS> s_vfs;
static std::unique_ptr<AfxScripting> s_env;
static bool s_tried = false;
// what the server said before the scripts were up (its data comes at join)
static std::vector<std::string> s_pending;
// what it said once and won't say again, for a reload: the game, its
// creatures, the latest weather
static std::map<std::string, std::string> s_once;
static std::string s_last_world;
static bool s_reload = false;
static bool s_ever_started = false; // events from before the first start wait; later ones don't
static bool s_failed_seen = false;

ScriptContext &scriptContext() { return s_ctx; }

static void stopEnv(); // below

static void start(Client *client, ISoundManager *sound)
{
	s_tried = true;
	s_ever_started = true;
	s_failed_seen = false;
	s_ctx = ScriptContext();
	s_ctx.client = client;
	s_ctx.sound = sound;
	s_vfs = std::make_unique<ModVFS>();
	s_vfs->scanModIntoMemory(BUILTIN_MOD_NAME, Client::getBuiltinLuaPath());
	try {
		s_env = std::make_unique<AfxScripting>(client, s_vfs.get());
		s_env->load();
		infostream << "afx: ambience scripts running" << std::endl;
		for (const auto &[channel, msg] : s_once)
			scriptWorld(msg);
		if (!s_last_world.empty())
			scriptWorld(s_last_world);
		for (const std::string &m : s_pending)
			scriptWorld(m);
	} catch (std::exception &e) {
		errorstream << "afx: the ambience scripts failed to start: " << e.what() << std::endl;
		s_env.reset();
		// what it started before it failed
		for (auto &[h, _] : s_ctx.sounds) {
			sound->stopSound(h);
			sound->freeId(h, 1);
		}
		s_ctx.sounds.clear();
	}
	s_pending.clear();
}

void scriptStep(Client *client, ISoundManager *sound, float dtime)
{
	if (s_reload) {
		// from disk again: edit builtin/afx, /afx_reload, listen
		s_reload = false;
		stopEnv();
		start(client, sound);
		client->pushToChatQueue(new ChatMessage(s_env ? L"afx: ambience reloaded" :
				L"afx: the ambience failed to load (see the log); fix it and /afx_reload"));
	}
	if (!s_tried)
		start(client, sound);
	if (!s_env)
		return;
	s_ctx.client = client;
	s_ctx.sound = sound;
	s_ctx.time += dtime;

	// sounds that follow an object; gone objects take their sounds along
	auto &env = client->getEnv();
	for (auto it = s_ctx.sounds.begin(); it != s_ctx.sounds.end();) {
		if (!it->second.object) {
			++it;
			continue;
		}
		ClientActiveObject *o = env.getActiveObject(it->second.object);
		if (!o) {
			sound->fadeSound(it->first, 2.f, 0.f);
			sound->freeId(it->first, 1);
			it = s_ctx.sounds.erase(it);
			continue;
		}
		const v3f pos = o->getPosition() / BS;
		sound->updateSoundPosVel(it->first, pos, soundVelocity(it->second.object, pos, dtime));
		++it;
	}
	s_env->step(dtime);
	if (s_env->failed() && !s_failed_seen) {
		// too many errors: it has stopped; so do its sounds (/afx_reload
		// brings it back)
		s_failed_seen = true;
		for (auto &[h, _] : s_ctx.sounds) {
			sound->fadeSound(h, 2.f, 0.f);
			sound->freeId(h, 1);
		}
		s_ctx.sounds.clear();
		client->pushToChatQueue(new ChatMessage(
				L"afx: the ambience stopped after repeated errors (see the log); /afx_reload"));
	}
}

// the scripts and their sounds; what the server said stays
static void stopEnv()
{
	setScriptDebugText("");
	if (s_env) {
		s_env->shutdown();
		s_env.reset();
	}
	if (s_ctx.sound) {
		for (auto &[h, _] : s_ctx.sounds) {
			s_ctx.sound->stopSound(h);
			s_ctx.sound->freeId(h, 1);
		}
	}
	s_ctx = ScriptContext();
	s_vfs.reset();
	s_tried = false;
}

void scriptStop()
{
	// (what the server said stays: it said it once, and afx may come back on)
	stopEnv();
	s_reload = false;
}

void scriptShutdown()
{
	stopEnv();
	s_ever_started = false;
	s_once.clear();
	s_last_world.clear();
	s_pending.clear();
	s_reload = false;
}

void scriptReload() { s_reload = true; }

void scriptWorld(const std::string &state)
{
	bool cached = false;
	if (!state.empty() && state[0] == '@') {
		const std::string channel = state.substr(1, state.find(' ') - 1);
		if (channel == "game" || channel == "entities") {
			s_once[channel] = state;
			cached = true;
		}
	} else {
		s_last_world = state;
		cached = true;
	}
	if (!s_env) {
		// (what's cached above is told again at the start; events wait only
		// for the very first start, not while afx is off: those are old news)
		if (!s_ever_started && !cached && s_pending.size() < 64)
			s_pending.push_back(state);
		return;
	}
	if (!state.empty() && state[0] == '@') {
		const size_t sp = state.find(' ');
		const std::string channel = state.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
		s_env->onData(channel, sp == std::string::npos ? "" : state.substr(sp + 1));
	} else {
		s_env->onWorld(state);
	}
}

void scriptNode(const char *what, v3s16 pos, const std::string &node)
{
	if (s_env)
		s_env->onNode(what, pos, node);
}

bool scriptChat(const std::string &message)
{
	// (the engine's own: it has to work when the scripts are broken)
	if (message == "/afx_reload") {
		scriptReload();
		return true;
	}
	// with afx off the scripts don't run: /afx turns it back on
	if (!s_env && (message == "/afx" || message.rfind("/afx ", 0) == 0) && s_ctx.client) {
		g_settings->setBool("afx_enable", true);
		s_ctx.client->pushToChatQueue(new ChatMessage(
				L"afx: environmental audio on again; /afx for the mixer"));
		return true;
	}
	return s_env && s_env->onChat(message);
}

bool scriptFormspec(const std::string &formname, const StringMap &fields)
{
	if (!s_env)
		return false;
	std::map<std::string, std::string> f(fields.begin(), fields.end());
	return s_env->onFormspec(formname, f);
}

bool soundAvailable(ISoundManager *sound, const std::string &name)
{
	// (the manager also looks in the engine's own sounds, share/sounds/afx)
	return sound && sound->hasSoundGroup(name);
}

static Settings *storage()
{
	static std::unique_ptr<Settings> s;
	if (!s) {
		s = std::make_unique<Settings>();
		const std::string dir = porting::path_user + DIR_DELIM "client";
		fs::CreateAllDirs(dir);
		s->readConfigFile((dir + DIR_DELIM "afx_storage.conf").c_str());
	}
	return s.get();
}

std::string storageGet(const std::string &key)
{
	std::string v;
	storage()->getNoEx(key, v);
	return v;
}

void storageSet(const std::string &key, const std::string &value)
{
	Settings *s = storage();
	if (value.empty())
		s->remove(key);
	else
		s->set(key, value);
	s->updateConfigFile((porting::path_user + DIR_DELIM "client" DIR_DELIM "afx_storage.conf").c_str());
}

} // namespace afx
