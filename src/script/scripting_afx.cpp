// Luanti "afx" fork: the client's ambience runtime
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "scripting_afx.h"
#include "client/client.h"
#include "cpp_api/s_internal.h"
#include "common/c_converter.h"
#include "lua_api/l_afx.h"
#include "lua_api/l_item.h"
#include "lua_api/l_noise.h"
#include "lua_api/l_util.h"
#include "lua_api/l_settings.h"
#include "log.h"
#include "porting.h"

AfxScripting::AfxScripting(Client *client, ModVFS *vfs) :
	ScriptApiBase(ScriptingType::Client)
{
	setGameDef(client);
	setModVFS(vfs);

	SCRIPTAPI_PRECHECKHEADER

	initializeSecurityClient();

	lua_getglobal(L, "core");
	int top = lua_gettop(L);
	LuaSettings::Register(L);
	LuaPcgRandom::Register(L);
	ModApiUtil::InitializeClient(L, top);
	ModApiItem::InitializeClient(L, top);
	ModApiAfx::Initialize(L, top);
	lua_pop(L, 1);

	lua_pushstring(L, "afx");
	lua_setglobal(L, "INIT");
}

void AfxScripting::load()
{
	loadModFromMemory(BUILTIN_MOD_NAME);
}

bool AfxScripting::pushHook(lua_State *L, const char *name)
{
	lua_getglobal(L, "core");
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
	if (lua_isfunction(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

bool AfxScripting::call(lua_State *L, int nargs, int nresults, const char *what)
{
	// function and args are on the stack; the handler goes under them
	const int base = lua_gettop(L) - nargs;
	PUSH_ERROR_HANDLER(L);
	lua_insert(L, base);
	if (lua_pcall(L, nargs, nresults, base) != 0) {
		const char *msg = lua_tostring(L, -1);
		errorstream << "afx: Lua error in " << what << ": " << (msg ? msg : "?") << std::endl;
		lua_settop(L, base - 1);
		// a rare error now and then is forgiven; a run of them stops it
		const u64 now = porting::getTimeMs();
		if (now - m_last_error_ms > 60000)
			m_errors = 0;
		m_last_error_ms = now;
		if (++m_errors == 21)
			errorstream << "afx: too many errors, the ambience scripts stop" << std::endl;
		return false;
	}
	lua_remove(L, base); // the handler; the results stay on top
	return true;
}

void AfxScripting::step(float dtime)
{
	if (failed())
		return;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_step"))
		return;
	lua_pushnumber(L, dtime);
	call(L, 1, 0, "step");
}

void AfxScripting::onWorld(const std::string &kv)
{
	if (failed())
		return;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_on_world"))
		return;
	lua_pushlstring(L, kv.data(), kv.size());
	call(L, 1, 0, "world");
}

void AfxScripting::onData(const std::string &channel, const std::string &data)
{
	if (failed())
		return;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_on_data"))
		return;
	lua_pushlstring(L, channel.data(), channel.size());
	lua_pushlstring(L, data.data(), data.size());
	call(L, 2, 0, "data");
}

void AfxScripting::onNode(const char *what, v3s16 pos, const std::string &node)
{
	if (failed())
		return;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_on_node"))
		return;
	lua_pushstring(L, what);
	push_v3s16(L, pos);
	lua_pushlstring(L, node.data(), node.size());
	call(L, 3, 0, "node event");
}

bool AfxScripting::onChat(const std::string &message)
{
	if (failed())
		return false;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_on_chat"))
		return false;
	lua_pushlstring(L, message.data(), message.size());
	if (!call(L, 1, 1, "chat"))
		return false;
	bool handled = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return handled;
}

bool AfxScripting::onFormspec(const std::string &formname,
		const std::map<std::string, std::string> &fields)
{
	if (failed())
		return false;
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_on_formspec"))
		return false;
	lua_pushlstring(L, formname.data(), formname.size());
	lua_createtable(L, 0, fields.size());
	for (const auto &[k, v] : fields) {
		lua_pushlstring(L, v.data(), v.size());
		lua_setfield(L, -2, k.c_str());
	}
	if (!call(L, 2, 1, "formspec"))
		return false;
	bool handled = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return handled;
}

void AfxScripting::shutdown()
{
	SCRIPTAPI_PRECHECKHEADER
	if (!pushHook(L, "afx_shutdown"))
		return;
	call(L, 0, 0, "shutdown");
}
