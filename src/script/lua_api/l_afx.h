// Luanti "afx" fork: the API of the client's ambience runtime
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// What the ambience scripts (builtin/afx) can see and do on the client:
// the loaded map and its node definitions, the local player and the
// objects around, what the engine hears, and sounds of their own.
// Names follow the server API where the meaning is the same, so the
// scripts read like a mod.

#pragma once

#include "lua_api/l_env.h"

class ModApiAfx : public ModApiEnvBase
{
private:
	static int l_get_builtin_path(lua_State *L);
	static int l_get_current_modname(lua_State *L);
	static int l_get_modpath(lua_State *L);

	// The loaded map
	static int l_get_node_or_nil(lua_State *L);
	static int l_get_node(lua_State *L);
	static int l_find_nodes_in_area(lua_State *L);
	static int l_find_nodes_in_area_under_air(lua_State *L);
	static int l_get_node_light(lua_State *L);
	static int l_get_natural_light(lua_State *L);
	static int l_get_timeofday(lua_State *L);
	// afx_node_defs() -> {name = {groups, drawtype, walkable, ...}}
	static int l_afx_node_defs(lua_State *L);

	// Sounds: handles are integers
	static int l_sound_play(lua_State *L);
	static int l_sound_fade(lua_State *L);
	static int l_sound_stop(lua_State *L);
	static int l_afx_sound_move(lua_State *L);
	static int l_afx_has_sound(lua_State *L);
	static int l_afx_sound_alive(lua_State *L);

	// The player, the objects around, what the engine hears
	static int l_afx_player(lua_State *L);
	static int l_afx_objects(lua_State *L);
	static int l_afx_hearing(lua_State *L);
	// afx_set_world({wind=, wind_x=, rain=, ...}): the weather the engine
	// bends distant sounds with
	static int l_afx_set_world(lua_State *L);

	// Storage, UI, the server
	static int l_afx_storage_get(lua_State *L);
	static int l_afx_storage_set(lua_State *L);
	static int l_show_formspec(lua_State *L);
	static int l_display_chat_message(lua_State *L);
	static int l_afx_server(lua_State *L);
	// afx_reload(): load the scripts again from disk at the next step
	static int l_afx_reload(lua_State *L);
	// afx_flash("lightning"|"blast", pos, strength, sky): the picture's side
	// of an event the ambience hears (fx renderer)
	static int l_afx_flash(lua_State *L);

public:
	static void Initialize(lua_State *L, int top);
};
