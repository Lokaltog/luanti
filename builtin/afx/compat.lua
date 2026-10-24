-- The server API the ambience scripts are written against, on the client:
-- the local player as the one connected player, the objects around with
-- what the server told us about their kind, callbacks, chat commands and
-- formspecs. Plus the world facts from the server (afx.world, afx.game).

local afx = {}
core.afx = afx

--[[ Callbacks ]]

local steps, loaded, punch_cbs, dig_cbs, fields_cbs, data_cbs, shutdown_cbs = {}, {}, {}, {}, {}, {}, {}
function core.register_globalstep(fn) steps[#steps + 1] = fn end
function core.register_on_mods_loaded(fn) loaded[#loaded + 1] = fn end
function core.register_on_punchnode(fn) punch_cbs[#punch_cbs + 1] = fn end
function core.register_on_dignode(fn) dig_cbs[#dig_cbs + 1] = fn end
function core.register_on_player_receive_fields(fn) fields_cbs[#fields_cbs + 1] = fn end
function core.register_on_leaveplayer() end -- the one player never leaves
function core.register_on_shutdown(fn) shutdown_cbs[#shutdown_cbs + 1] = fn end
-- fn(data): a data channel from the server ("@channel payload")
function afx.register_on_data(channel, fn)
	data_cbs[channel] = data_cbs[channel] or {}
	table.insert(data_cbs[channel], fn)
end

--[[ Nodes ]]

core.registered_nodes = core.afx_node_defs()
core.registered_items = core.registered_nodes
core.registered_aliases = {}
core.registered_entities = {} -- filled from the server's @entities

function core.get_item_group(name, group)
	local def = core.registered_nodes[name]
	return def and def.groups[group] or 0
end

function core.hash_node_position(p)
	return (p.z + 32768) * 65536 * 65536 + (p.y + 32768) * 65536 + p.x + 32768
end

--[[ What the server told us ]]

-- afx.world: the latest "key=value" state (weather, biome...), numbers
-- where they parse; afx.game: the game's facts (@game, once)
afx.world = {}
afx.game = {mods = {}, flags = {}}

function core.afx_on_world(kv)
	for k, v in kv:gmatch("([%w_]+)=(%S+)") do
		afx.world[k] = tonumber(v) or v
	end
end

afx.register_on_data("game", function(data)
	local g = core.parse_json(data)
	if type(g) ~= "table" then return end
	afx.game = g
	g.mods, g.flags = g.mods or {}, g.flags or {}
	local set = {}
	for _, m in ipairs(g.mods) do set[m] = true end
	g.mod_set = set
end)

afx.register_on_data("entities", function(data)
	local t = core.parse_json(data)
	if type(t) ~= "table" then return end
	for name, def in pairs(t) do
		def.name = name
		core.registered_entities[name] = def
	end
end)

function core.afx_on_data(channel, data)
	for _, fn in ipairs(data_cbs[channel] or {}) do fn(data) end
end

function core.get_modpath(name)
	if afx.game.mod_set and afx.game.mod_set[name] then return name .. ":" end
	return nil
end
function core.get_modnames() return afx.game.mods or {} end
function core.get_mapgen_setting(name)
	if name == "water_level" then return tostring(afx.game.water_level or 1) end
end
function core.get_game_info() return {id = afx.game.id or "?"} end
function core.get_day_count() return tonumber(afx.world.day) or 0 end
function core.get_biome_data()
	return afx.world.biome and {biome = afx.world.biome,
		heat = tonumber(afx.world.heat), humidity = tonumber(afx.world.humidity)} or nil
end
function core.get_biome_name(b) return type(b) == "string" and b or "" end
function core.is_singleplayer() return core.afx_server()["local"] end
function core.check_player_privs() return true end

-- Time since the ambience started: the slow cycles (wind veering) run on it
local time = 0
function core.get_gametime() return math.floor(time) end

--[[ The local player ]]

local P = {} -- refreshed every step from core.afx_player()
local meta_cache
local function storage_prefix()
	local s = core.afx_server()
	return ((s.address ~= "" and s.address) or "local") .. "." .. tostring(afx.game.world or "") ..
		"." .. tostring(P.name or "")
end

local player = {}
function player:get_player_name() return P.name or "singleplayer" end
function player:is_player() return true end
function player:get_pos() return vector.copy(P.pos or vector.zero()) end
function player:get_velocity() return vector.copy(P.velocity or vector.zero()) end
function player:get_hp() return P.hp or 20 end
function player:get_breath() return P.breath or 10 end
function player:get_look_horizontal() return P.yaw and math.rad(P.yaw) or 0 end
function player:get_properties()
	return {hp_max = P.hp_max or 20, breath_max = (P.breath_max or 0) > 0 and P.breath_max or 10,
		eye_height = P.eye_height or 1.6}
end
function player:get_wielded_item()
	local name = P.wield or ""
	return {get_name = function() return name end}
end
function player:get_acoustics() return core.afx_hearing() end
function player:set_afx_world() end -- the engine hears the server directly
function player:get_meta()
	if not meta_cache then
		meta_cache = {
			get_string = function(_, k)
				return core.afx_storage_get(storage_prefix() .. "." .. k) or ""
			end,
			set_string = function(_, k, v)
				core.afx_storage_set(storage_prefix() .. "." .. k, v)
			end,
		}
	end
	return meta_cache
end
afx.player = player

function core.get_connected_players() return P.name and {player} or {} end
function core.get_player_by_name(name) return name == P.name and player or nil end

--[[ Objects around ]]

local obj_mt = {__index = {}}
local O = obj_mt.__index
function O:get_pos() return vector.copy(self._pos) end
function O:get_velocity() return vector.copy(self._vel) end
function O:is_player() return self._player end
function O:get_properties() return {visual_size = self._visual_size} end
-- what the scripts know of its kind: the server's entity table, and a
-- guess at the young (smaller than the kind's own size)
function O:get_luaentity()
	if self._player then return nil end
	if self._ent == nil then
		local def = core.registered_entities[self._name]
		local ent = {name = self._name}
		if def then
			for k, v in pairs(def) do ent[k] = v end
			local base = def.visual_size and def.visual_size.x
			if base and self._visual_size and self._visual_size.x < base * 0.75 then
				ent.child = true
			end
		end
		self._ent = ent
	end
	return self._ent
end
afx.object_mt = obj_mt

local function wrap(o)
	return setmetatable({_id = o.id, _name = o.name or "", _pos = o.pos, _vel = o.velocity,
		_player = o.is_player, _visual_size = o.visual_size, _height = o.height,
		_texture = o.texture, _mesh = o.mesh}, obj_mt)
end

function core.get_objects_inside_radius(pos, r)
	local out = {}
	for _, o in ipairs(core.afx_objects(pos, r)) do out[#out + 1] = wrap(o) end
	return out
end
afx.wrap_object = wrap

--[[ Sounds: "object =" is an object from get_objects_inside_radius ]]

local native_play = core.sound_play
function core.sound_play(spec, params, ephemeral)
	if params and params.object then
		local o = params.object
		params.object = type(o) == "table" and o._id or o
	end
	return native_play(spec, params, ephemeral)
end

--[[ Chat commands and formspecs ]]

local commands = {}
function core.register_chatcommand(name, def) commands[name] = def end

function core.afx_on_chat(msg)
	local cmd, param = msg:match("^/([%w_]+)%s*(.*)$")
	local def = cmd and commands[cmd]
	if not def then return false end
	local ok, text = def.func(player:get_player_name(), param or "")
	if text then core.display_chat_message(text) end
	return true
end

-- formspecs it shows are named "afx:..." so the engine hands their fields
-- back here
local native_show = core.show_formspec
function core.show_formspec(_, formname, spec)
	native_show(formname, spec)
end

function core.afx_on_formspec(formname, fields)
	for _, fn in ipairs(fields_cbs) do
		if fn(player, formname, fields) then break end
	end
	return true
end

--[[ Node events ]]

function core.afx_on_node(what, pos, name)
	local node = {name = name, param1 = 0, param2 = 0}
	for _, fn in ipairs(what == "dig" and dig_cbs or what == "punch" and punch_cbs or {}) do
		fn(pos, node, player)
	end
end

--[[ The step ]]

function core.afx_mods_loaded()
	for _, fn in ipairs(loaded) do fn() end
end

function core.afx_step(dtime)
	time = time + dtime
	local p = core.afx_player()
	if p then
		for k, v in pairs(p) do P[k] = v end
	end
	if not P.name then return end
	for _, fn in ipairs(steps) do fn(dtime) end
end

function core.afx_shutdown()
	for _, fn in ipairs(shutdown_cbs) do fn() end
end

-- Unused server functions some code checks for
core.get_worldpath = function() return nil end

--[[ Development: afx_debug_eval (a line of Lua), run in here a while after
start, its result logged. For headless tests of the ambience. ]]

do
	local code = core.settings:get("afx_debug_eval")
	if code and code ~= "" then
		local at = tonumber(core.settings:get("afx_debug_eval_at")) or 12
		core.register_globalstep(function()
			if not at or time < at then return end
			at = nil
			local fn, err = loadstring(code, "=afx_debug_eval")
			if not fn then core.log("error", "afx_debug_eval: " .. err) return end
			local ok, res = pcall(fn)
			core.log("action", "afx_debug_eval -> " .. tostring(ok) .. " " ..
				(type(res) == "table" and core.write_json(res) or tostring(res)))
		end)
	end
end
