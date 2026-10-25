// Luanti "afx" fork: the API of the client's ambience runtime
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "l_afx.h"
#include "l_internal.h"
#include "common/c_content.h"
#include "common/c_converter.h"
#include "cpp_api/s_base.h"
#include "cpp_api/s_node.h"
#include "client/afx/afx.h"
#include "client/afx/afx_script.h"
#include "client/fx/fx.h"
#include "chatmessage.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientevent.h"
#include "client/clientmap.h"
#include "client/content_cao.h"
#include "client/localplayer.h"
#include "client/sound.h"
#include "daynightratio.h"
#include "itemdef.h"
#include "nodedef.h"
#include "sound_spec.h"
#include "util/string.h"
#include <sstream>

static Client *afxClient() { return afx::scriptContext().client; }
static ISoundManager *afxSound() { return afx::scriptContext().sound; }

int ModApiAfx::l_get_builtin_path(lua_State *L)
{
	lua_pushstring(L, BUILTIN_MOD_NAME ":");
	return 1;
}

int ModApiAfx::l_get_current_modname(lua_State *L)
{
	std::string s = ScriptApiBase::getCurrentModNameInsecure(L);
	if (s.empty())
		lua_pushnil(L);
	else
		lua_pushstring(L, s.c_str());
	return 1;
}

int ModApiAfx::l_get_modpath(lua_State *L)
{
	std::string path = readParam<std::string>(L, 1) + ":";
	lua_pushstring(L, path.c_str());
	return 1;
}

/* The loaded map */

static bool nodeAt(v3s16 p, MapNode &n)
{
	bool ok = false;
	n = afxClient()->getEnv().getMap().getNode(p, &ok);
	return ok;
}

int ModApiAfx::l_get_node_or_nil(lua_State *L)
{
	MapNode n;
	if (!nodeAt(read_v3s16(L, 1), n))
		lua_pushnil(L);
	else
		pushnode(L, n);
	return 1;
}

int ModApiAfx::l_get_node(lua_State *L)
{
	MapNode n;
	if (!nodeAt(read_v3s16(L, 1), n))
		n = MapNode(CONTENT_IGNORE);
	pushnode(L, n);
	return 1;
}

// A filter as a lookup table by content id
static std::vector<bool> contentFilter(lua_State *L, int idx, const NodeDefManager *ndef)
{
	std::vector<content_t> ids;
	if (lua_istable(L, idx)) {
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			if (lua_isstring(L, -1))
				ndef->getIds(lua_tostring(L, -1), ids);
			lua_pop(L, 1);
		}
	} else if (lua_isstring(L, idx)) {
		ndef->getIds(lua_tostring(L, idx), ids);
	}
	std::vector<bool> f;
	for (content_t c : ids) {
		if (c >= f.size())
			f.resize(c + 1, false);
		f[c] = true;
	}
	return f;
}

static inline bool inFilter(const std::vector<bool> &f, content_t c)
{
	return c < f.size() && f[c];
}

int ModApiAfx::l_find_nodes_in_area(lua_State *L)
{
	v3s16 minp = read_v3s16(L, 1), maxp = read_v3s16(L, 2);
	sortBoxVerticies(minp, maxp);
	checkArea(minp, maxp);
	const NodeDefManager *ndef = afxClient()->ndef();
	const std::vector<bool> filter = contentFilter(L, 3, ndef);

	lua_newtable(L);
	const int list = lua_gettop(L);
	std::unordered_map<content_t, u32> counts;
	u32 i = 0;
	if (!filter.empty()) {
		afxClient()->getEnv().getMap().forEachNodeInArea(minp, maxp,
				[&](v3s16 p, MapNode n) -> bool {
			const content_t c = n.getContent();
			if (inFilter(filter, c)) {
				push_v3s16(L, p);
				lua_rawseti(L, list, ++i);
				counts[c]++;
			}
			return true;
		});
	}
	lua_createtable(L, 0, counts.size());
	for (const auto &[c, n] : counts) {
		lua_pushinteger(L, n);
		lua_setfield(L, -2, ndef->get(c).name.c_str());
	}
	return 2;
}

int ModApiAfx::l_find_nodes_in_area_under_air(lua_State *L)
{
	v3s16 minp = read_v3s16(L, 1), maxp = read_v3s16(L, 2);
	sortBoxVerticies(minp, maxp);
	checkArea(minp, maxp);
	const NodeDefManager *ndef = afxClient()->ndef();
	const std::vector<bool> filter = contentFilter(L, 3, ndef);
	Map &map = afxClient()->getEnv().getMap();

	lua_newtable(L);
	u32 i = 0;
	if (filter.empty())
		return 1;
	v3s16 p;
	for (p.X = minp.X; p.X <= maxp.X; p.X++)
	for (p.Z = minp.Z; p.Z <= maxp.Z; p.Z++) {
		p.Y = minp.Y;
		content_t c = map.getNode(p).getContent();
		for (; p.Y <= maxp.Y; p.Y++) {
			const content_t above = map.getNode(v3s16(p.X, p.Y + 1, p.Z)).getContent();
			if (c != CONTENT_AIR && above == CONTENT_AIR && inFilter(filter, c)) {
				push_v3s16(L, p);
				lua_rawseti(L, -2, ++i);
			}
			c = above;
		}
	}
	return 1;
}

static float timeOfDay(lua_State *L, int idx)
{
	if (lua_isnumber(L, idx))
		return lua_tonumber(L, idx);
	return afxClient()->getEnv().getTimeOfDayF();
}

int ModApiAfx::l_get_node_light(lua_State *L)
{
	MapNode n;
	if (!nodeAt(read_v3s16(L, 1), n)) {
		lua_pushnil(L);
		return 1;
	}
	const NodeDefManager *ndef = afxClient()->ndef();
	const u32 dnr = time_to_daynight_ratio(timeOfDay(L, 2) * 24000.f, true);
	lua_pushinteger(L, n.getLightBlend(dnr, ndef->getLightingFlags(n)));
	return 1;
}

int ModApiAfx::l_get_natural_light(lua_State *L)
{
	MapNode n;
	if (!nodeAt(read_v3s16(L, 1), n)) {
		lua_pushnil(L);
		return 1;
	}
	const NodeDefManager *ndef = afxClient()->ndef();
	const u8 daylight = n.getLight(LIGHTBANK_DAY, ndef->getLightingFlags(n));
	const u32 dnr = time_to_daynight_ratio(timeOfDay(L, 2) * 24000.f, true);
	lua_pushinteger(L, dnr * daylight / 1000);
	return 1;
}

int ModApiAfx::l_get_timeofday(lua_State *L)
{
	lua_pushnumber(L, afxClient()->getEnv().getTimeOfDayF());
	return 1;
}

static void pushSound(lua_State *L, const SoundSpec &s)
{
	if (!s.exists()) {
		lua_pushnil(L);
		return;
	}
	lua_createtable(L, 0, 3);
	lua_pushstring(L, s.name.c_str());
	lua_setfield(L, -2, "name");
	lua_pushnumber(L, s.gain);
	lua_setfield(L, -2, "gain");
	lua_pushnumber(L, s.pitch);
	lua_setfield(L, -2, "pitch");
}

int ModApiAfx::l_afx_node_defs(lua_State *L)
{
	const NodeDefManager *ndef = afxClient()->ndef();
	lua_newtable(L);
	for (u32 c = 0; c < ndef->size(); c++) {
		const ContentFeatures &f = ndef->get((content_t)c);
		if (f.name.empty() || (c == CONTENT_IGNORE || c == CONTENT_UNKNOWN))
			continue;
		lua_createtable(L, 0, 12);
		lua_pushstring(L, f.name.c_str());
		lua_setfield(L, -2, "name");
		push_groups(L, f.groups);
		lua_setfield(L, -2, "groups");
		lua_pushstring(L, enum_to_string(ScriptApiNode::es_DrawType, f.drawtype));
		lua_setfield(L, -2, "drawtype");
		lua_pushstring(L, enum_to_string(ScriptApiNode::es_LiquidType, f.liquid_type));
		lua_setfield(L, -2, "liquidtype");
		lua_pushstring(L, enum_to_string(ScriptApiNode::es_ContentParamType2, f.param_type_2));
		lua_setfield(L, -2, "paramtype2");
		lua_pushboolean(L, f.walkable);
		lua_setfield(L, -2, "walkable");
		lua_pushboolean(L, f.climbable);
		lua_setfield(L, -2, "climbable");
		lua_pushboolean(L, f.sunlight_propagates);
		lua_setfield(L, -2, "sunlight_propagates");
		lua_pushboolean(L, f.buildable_to);
		lua_setfield(L, -2, "buildable_to");
		lua_pushinteger(L, f.light_source);
		lua_setfield(L, -2, "light_source");
		lua_createtable(L, 0, 3);
		pushSound(L, f.sound_footstep);
		lua_setfield(L, -2, "footstep");
		pushSound(L, f.sound_dig);
		lua_setfield(L, -2, "dig");
		pushSound(L, f.sound_dug);
		lua_setfield(L, -2, "dug");
		lua_setfield(L, -2, "sounds");
		lua_setfield(L, -2, f.name.c_str());
	}
	return 1;
}

/* Sounds */

static v3f readPos(lua_State *L, int idx)
{
	return read_v3f(L, idx);
}

// sound_play(name or {name=, gain=, pitch=}, params, ephemeral) -> handle
// params: gain, pitch, loop, pos, object (an object id), start_time,
// max_hear_distance (not played when farther, at the start)
int ModApiAfx::l_sound_play(lua_State *L)
{
	ISoundManager *sound = afxSound();
	if (!sound)
		return 0;
	SoundSpec spec;
	if (lua_isstring(L, 1)) {
		spec.name = lua_tostring(L, 1);
	} else if (lua_istable(L, 1)) {
		getstringfield(L, 1, "name", spec.name);
		getfloatfield(L, 1, "gain", spec.gain);
		getfloatfield(L, 1, "pitch", spec.pitch);
	}
	if (spec.name.empty())
		return 0;
	bool positional = false;
	v3f pos, vel;
	u16 object = 0;
	float range = 0.f;
	if (lua_istable(L, 2)) {
		float gain = 1.f, pitch = 1.f;
		getfloatfield(L, 2, "gain", gain);
		getfloatfield(L, 2, "pitch", pitch);
		spec.gain *= gain;
		spec.pitch *= pitch;
		getboolfield(L, 2, "loop", spec.loop);
		getfloatfield(L, 2, "start_time", spec.start_time);
		getfloatfield(L, 2, "max_hear_distance", range);
		lua_getfield(L, 2, "pos");
		if (lua_istable(L, -1)) {
			pos = readPos(L, -1);
			positional = true;
		}
		lua_pop(L, 1);
		int obj = 0;
		if (getintfield(L, 2, "object", obj) && obj > 0) {
			ClientActiveObject *o = afxClient()->getEnv().getActiveObject(obj);
			if (!o)
				return 0;
			object = obj;
			pos = o->getPosition() / BS;
			vel = o->getVelocity() / BS;
			positional = true;
		}
	}
	const bool ephemeral = lua_toboolean(L, 3);
	if (positional && range > 0.f) {
		LocalPlayer *lp = afxClient()->getEnv().getLocalPlayer();
		if (lp && (lp->getPosition() / BS).getDistanceFrom(pos) > range)
			return 0;
	}
	if (ephemeral) {
		if (positional)
			sound->playSoundAt(0, spec, pos, vel);
		else
			sound->playSound(0, spec);
		return 0;
	}
	// two owners: the sound itself and us (freed when the script stops it)
	const sound_handle_t h = sound->allocateId(2);
	if (positional)
		sound->playSoundAt(h, spec, pos, vel);
	else
		sound->playSound(h, spec);
	afx::scriptContext().sounds[h] = {object, spec.loop};
	lua_pushinteger(L, h);
	return 1;
}

static void forget(sound_handle_t h)
{
	auto &sounds = afx::scriptContext().sounds;
	auto it = sounds.find(h);
	if (it == sounds.end())
		return;
	sounds.erase(it);
	afxSound()->freeId(h, 1);
}

int ModApiAfx::l_sound_fade(lua_State *L)
{
	ISoundManager *sound = afxSound();
	const sound_handle_t h = luaL_checkinteger(L, 1);
	const float step = luaL_checknumber(L, 2), gain = luaL_checknumber(L, 3);
	if (!sound || !afx::scriptContext().sounds.count(h))
		return 0;
	sound->fadeSound(h, step, gain);
	// faded out: the manager stops it at the end; it's no longer ours
	if (gain <= 0.f)
		forget(h);
	return 0;
}

int ModApiAfx::l_sound_stop(lua_State *L)
{
	ISoundManager *sound = afxSound();
	const sound_handle_t h = luaL_checkinteger(L, 1);
	if (!sound || !afx::scriptContext().sounds.count(h))
		return 0;
	sound->stopSound(h);
	forget(h);
	return 0;
}

// afx_sound_move(handle, pos, vel): a moving emitter (a gust sweeping by)
int ModApiAfx::l_afx_sound_move(lua_State *L)
{
	ISoundManager *sound = afxSound();
	const sound_handle_t h = luaL_checkinteger(L, 1);
	if (!sound || !afx::scriptContext().sounds.count(h))
		return 0;
	const v3f pos = read_v3f(L, 2);
	const v3f vel = lua_istable(L, 3) ? read_v3f(L, 3) : v3f();
	sound->updateSoundPosVel(h, pos, vel);
	return 0;
}

int ModApiAfx::l_afx_has_sound(lua_State *L)
{
	lua_pushboolean(L, afx::soundAvailable(afxSound(), luaL_checkstring(L, 1)));
	return 1;
}

/* The player, the objects around, what the engine hears */

static void setnum(lua_State *L, const char *k, double v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, -2, k);
}

static void setbool(lua_State *L, const char *k, bool v)
{
	lua_pushboolean(L, v);
	lua_setfield(L, -2, k);
}

int ModApiAfx::l_afx_player(lua_State *L)
{
	Client *client = afxClient();
	LocalPlayer *p = client->getEnv().getLocalPlayer();
	if (!p)
		return 0;
	lua_createtable(L, 0, 16);
	push_v3f(L, p->getPosition() / BS);
	lua_setfield(L, -2, "pos");
	ClientActiveObject *parent = p->getParent();
	push_v3f(L, (parent ? parent->getVelocity() : p->getSpeed()) / BS);
	lua_setfield(L, -2, "velocity");
	setnum(L, "yaw", p->getYaw());
	setnum(L, "pitch", p->getPitch());
	setnum(L, "eye_height", p->getEyeOffset().Y / BS);
	setnum(L, "hp", p->hp);
	setnum(L, "breath", p->getBreath());
	setbool(L, "in_liquid", p->in_liquid);
	setbool(L, "on_ground", p->touching_ground);
	setbool(L, "climbing", p->is_climbing);
	setbool(L, "attached", parent != nullptr);
	GenericCAO *cao = p->getCAO();
	if (cao) {
		setnum(L, "hp_max", cao->getProperties().hp_max);
		setnum(L, "breath_max", cao->getProperties().breath_max);
	}
	ItemStack selected, hand;
	p->getWieldedItem(&selected, &hand);
	lua_pushstring(L, selected.name.c_str());
	lua_setfield(L, -2, "wield");
	lua_pushstring(L, p->getName().c_str());
	lua_setfield(L, -2, "name");
	return 1;
}

// afx_objects(pos, radius) -> {{id, name, pos, velocity, is_player, ...}}
int ModApiAfx::l_afx_objects(lua_State *L)
{
	Client *client = afxClient();
	const v3f origin = read_v3f(L, 1);
	const float r = luaL_checknumber(L, 2);
	std::vector<DistanceSortedActiveObject> objs;
	client->getEnv().getActiveObjects(origin * BS, r * BS, objs);
	lua_createtable(L, objs.size(), 0);
	int i = 0;
	for (auto &d : objs) {
		ClientActiveObject *o = d.obj;
		if (!o || o->isLocalPlayer())
			continue;
		GenericCAO *g = dynamic_cast<GenericCAO *>(o);
		lua_createtable(L, 0, 10);
		setnum(L, "id", o->getId());
		push_v3f(L, o->getPosition() / BS);
		lua_setfield(L, -2, "pos");
		push_v3f(L, o->getVelocity() / BS);
		lua_setfield(L, -2, "velocity");
		setbool(L, "attached", o->getParent() != nullptr);
		if (g) {
			lua_pushstring(L, g->getName().c_str());
			lua_setfield(L, -2, "name");
			setbool(L, "is_player", g->isPlayer());
			const ObjectProperties &prop = g->getProperties();
			push_v3f(L, prop.visual_size);
			lua_setfield(L, -2, "visual_size");
			lua_pushstring(L, prop.mesh.c_str());
			lua_setfield(L, -2, "mesh");
			lua_pushstring(L, prop.textures.empty() ? "" : prop.textures[0].c_str());
			lua_setfield(L, -2, "texture");
			setnum(L, "height", (prop.collisionbox.MaxEdge.Y - prop.collisionbox.MinEdge.Y));
			setbool(L, "physical", prop.physical);
		}
		lua_rawseti(L, -2, ++i);
	}
	return 1;
}

int ModApiAfx::l_afx_hearing(lua_State *L)
{
	const auto h = afx::hearing(afxSound());
	if (h.empty())
		return 0;
	lua_createtable(L, 0, h.size());
	for (const auto &[k, v] : h)
		setnum(L, k.c_str(), v);
	return 1;
}

int ModApiAfx::l_afx_set_world(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	std::ostringstream os;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_type(L, -2) == LUA_TSTRING && lua_isnumber(L, -1))
			os << lua_tostring(L, -2) << "=" << lua_tonumber(L, -1) << " ";
		lua_pop(L, 1);
	}
	afx::setWorldState(os.str());
	return 0;
}

/* Storage, UI, the server */

static std::string storageKey(lua_State *L, int idx)
{
	std::string k = luaL_checkstring(L, idx);
	for (char &c : k) {
		if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-'))
			c = '_';
	}
	return "afx." + k;
}

int ModApiAfx::l_afx_storage_get(lua_State *L)
{
	const std::string v = afx::storageGet(storageKey(L, 1));
	if (v.empty())
		return 0;
	lua_pushlstring(L, v.data(), v.size());
	return 1;
}

int ModApiAfx::l_afx_storage_set(lua_State *L)
{
	afx::storageSet(storageKey(L, 1), lua_isstring(L, 2) ? lua_tostring(L, 2) : "");
	return 0;
}

int ModApiAfx::l_show_formspec(lua_State *L)
{
	ClientEvent *event = new ClientEvent();
	event->type = CE_SHOW_CSM_FORMSPEC; // routed back to us by its "afx:" name
	event->show_formspec.formname = new std::string(luaL_checkstring(L, 1));
	event->show_formspec.formspec = new std::string(luaL_checkstring(L, 2));
	afxClient()->pushToEventQueue(event);
	return 0;
}

int ModApiAfx::l_display_chat_message(lua_State *L)
{
	afxClient()->pushToChatQueue(new ChatMessage(utf8_to_wide(luaL_checkstring(L, 1))));
	return 0;
}

int ModApiAfx::l_afx_server(lua_State *L)
{
	Client *c = afxClient();
	lua_createtable(L, 0, 2);
	lua_pushstring(L, c->getAddressName().c_str());
	lua_setfield(L, -2, "address");
	setbool(L, "local", c->getAddressName().empty() || c->getAddressName() == "localhost" ||
			c->getAddressName() == "127.0.0.1" || c->getAddressName() == "::1");
	return 1;
}

int ModApiAfx::l_afx_flash(lua_State *L)
{
	const std::string kind = luaL_checkstring(L, 1);
	const v3f pos = read_v3f(L, 2);
	const float strength = luaL_optnumber(L, 3, 1.0);
	const float sky = luaL_optnumber(L, 4, 0.0);
	fx::addFlash(kind == "blast" ? fx::FlashKind::Blast : fx::FlashKind::Lightning, pos,
			strength, sky);
	return 0;
}

int ModApiAfx::l_afx_reload(lua_State *L)
{
	afx::scriptReload();
	return 0;
}

void ModApiAfx::Initialize(lua_State *L, int top)
{
	API_FCT(afx_reload);
	API_FCT(afx_flash);
	API_FCT(get_builtin_path);
	API_FCT(get_current_modname);
	API_FCT(get_modpath);
	API_FCT(get_node_or_nil);
	API_FCT(get_node);
	API_FCT(find_nodes_in_area);
	API_FCT(find_nodes_in_area_under_air);
	API_FCT(get_node_light);
	API_FCT(get_natural_light);
	API_FCT(get_timeofday);
	API_FCT(afx_node_defs);
	API_FCT(sound_play);
	API_FCT(sound_fade);
	API_FCT(sound_stop);
	API_FCT(afx_sound_move);
	API_FCT(afx_has_sound);
	API_FCT(afx_player);
	API_FCT(afx_objects);
	API_FCT(afx_hearing);
	API_FCT(afx_set_world);
	API_FCT(afx_storage_get);
	API_FCT(afx_storage_set);
	API_FCT(show_formspec);
	API_FCT(display_chat_message);
	API_FCT(afx_server);
}
