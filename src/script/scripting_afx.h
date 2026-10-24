// Luanti "afx" fork: the client's ambience runtime
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A Lua environment of its own on the client, separate from client-side
// modding: always on (with afx), not subject to enable_client_modding or a
// server's CSM restrictions, running only the engine's own scripts
// (builtin/afx). It decides what the world sounds like: beds, emitters at
// real nodes, accents. The server only tells it what a client can't know
// (weather, biome, the game's creatures).

#pragma once

#include "cpp_api/s_base.h"
#include "cpp_api/s_security.h"
#include "irr_v3d.h"
#include <map>
#include <string>

class Client;
struct ModVFS;

class AfxScripting : virtual public ScriptApiBase, public ScriptApiSecurity
{
public:
	AfxScripting(Client *client, ModVFS *vfs);

	/// Run builtin (common helpers, then builtin/afx). Throws ModError.
	void load();

	// Callbacks into the scripts. Lua errors are logged, not fatal: the
	// ambience is not worth losing the game over.
	void step(float dtime);
	void onWorld(const std::string &kv);
	void onData(const std::string &channel, const std::string &data);
	void onNode(const char *what, v3s16 pos, const std::string &node);
	bool onChat(const std::string &message);
	bool onFormspec(const std::string &formname, const std::map<std::string, std::string> &fields);
	void shutdown();

	bool failed() const { return m_errors > 20; }

protected:
	bool checkPathInternal(const std::string &, bool, bool *) override { return false; }
	bool modNamesAreTrusted() override { return true; }

private:
	// Push core.<name> (a function) or return false
	bool pushHook(lua_State *L, const char *name);
	bool call(lua_State *L, int nargs, int nresults, const char *what);
	int m_errors = 0;
};
