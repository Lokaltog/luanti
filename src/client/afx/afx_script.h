// Luanti "afx" fork: the client's ambience runtime, engine side
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Owns the ambience Lua environment (script/scripting_afx.h) and what its
// API needs between calls: the sounds it plays (with the objects they
// follow) and a little persistent storage.

#pragma once

#include "client/sound.h"
#include "irr_v3d.h"
#include "util/string.h"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Client;

namespace afx
{

struct ScriptSound {
	u16 object = 0;   // follow this active object (0: none)
	bool loop = false;
};

struct ScriptContext {
	Client *client = nullptr;
	ISoundManager *sound = nullptr;
	std::unordered_map<sound_handle_t, ScriptSound> sounds;
	float time = 0.f; // seconds since the environment started
};

/// Valid while the environment runs (during its callbacks).
ScriptContext &scriptContext();

/// Call every frame (from afx::update): starts the environment once the
/// game is up, then steps it.
void scriptStep(Client *client, ISoundManager *sound, float dtime);
/// afx turned off: the scripts and their sounds stop (what the server told
/// them is kept for when it comes back on).
void scriptStop();
/// The game ends: all of it goes.
void scriptShutdown();
/// Load the scripts from disk again at the next step (/afx_reload): their
/// sounds stop, what the server told them is told again.
void scriptReload();

/// From the server: weather and the like ("key=value ...", also parsed by
/// the engine), and data channels ("@channel payload").
void scriptWorld(const std::string &state);
/// Node interactions the player makes (punch, dig, place).
void scriptNode(const char *what, v3s16 pos, const std::string &node);
/// A chat line the player typed; true when the ambience handled it (it
/// isn't sent to the server).
bool scriptChat(const std::string &message);
/// Fields of a formspec it showed ("afx:..." names).
bool scriptFormspec(const std::string &formname, const StringMap &fields);

/// What the engine hears, as the key/value pairs it also tells the server.
std::vector<std::pair<std::string, float>> hearing(ISoundManager *sound);

/// Is there a sound by this name: media from the server, or the engine's
/// own sounds (share/sounds/afx)?
bool soundAvailable(ISoundManager *sound, const std::string &name);

/// Persistent per-user storage for the ambience (discoveries and the like).
std::string storageGet(const std::string &key);
void storageSet(const std::string &key, const std::string &value);

} // namespace afx
