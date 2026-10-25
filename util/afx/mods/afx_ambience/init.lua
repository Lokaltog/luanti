-- afx_ambience: the server's side of the afx fork's ambience.
--
-- The ambience itself runs on the client (the engine's builtin/afx): it
-- reads the map the client has, the objects around, what the engine hears,
-- and plays everything. This mod only tells it what a client can't know:
--
--   the weather   rain and snow where the player is (mcl_weather), the wind
--                 (slow swells and gusts, the same for everyone)
--   the place     the biome, its heat and humidity, the dimension
--   the game      its id, its mods, whether it plays its own water, lava and
--                 fire, sea level, the world, its creatures (type, voice)
--   events        lightning strikes, explosions, a gust rising
--
-- plus a few things to build (content.lua) and hooks for test scenarios.
-- Without the afx client, players just don't get any of it.

afx_ambience = {}
local A = afx_ambience

local function rand(a, b) return a + (b - a) * math.random() end

--[[ Weather ]]

-- Rain intensity 0..1 at a position. Forced with afx_ambience.set_rain().
local forced_rain
function A.set_rain(intensity)
	forced_rain = intensity
end

function A.rain_at(pos)
	if forced_rain then
		return forced_rain
	end
	local w = rawget(_G, "mcl_weather")
	if not w then
		return 0
	end
	local state = w.state or (w.get_weather and w.get_weather())
	if state ~= "rain" and state ~= "thunder" then
		return 0
	end
	if w.has_rain then
		local ok, has = pcall(w.has_rain, pos)
		if ok and not has then
			return 0 -- dry biome
		end
	end
	if w.has_snow then
		local ok, has = pcall(w.has_snow, pos)
		if ok and has then
			return 0 -- cold biome: it falls as snow
		end
	end
	return state == "thunder" and 1 or 0.7
end

-- Snowfall 0..1 here: snow weather, or rain falling as snow in a cold biome.
function A.snow_at(pos)
	if forced_rain then return 0 end
	local w = rawget(_G, "mcl_weather")
	if not w then return 0 end
	local state = w.state
	if state == "snow" then
		-- snow weather only snows where it's cold enough
		if not w.has_snow then return 1 end
		local ok, has = pcall(w.has_snow, pos)
		return (ok and has) and 1 or 0
	end
	if (state == "rain" or state == "thunder") and w.has_snow then
		local ok, has = pcall(w.has_snow, pos)
		if ok and has then return 1 end
	end
	return 0
end

--[[ Wind: two slow swells plus discrete gusts that rise over a few
seconds, hold and die away. Shared by everyone (it's the weather); the
client shapes it by how exposed the player is. ]]

local gust = {level = 0, phase = "idle", t = 0, next = 20, peak = 1}

local function gust_rises()
	-- the client sweeps a gust through the trees upwind of you
	for _, player in ipairs(core.get_connected_players()) do
		player:send_afx_data("gust", ("%.2f"):format(gust.peak))
	end
end

local function step_gust(dtime)
	gust.t = gust.t + dtime
	if gust.phase == "idle" then
		gust.next = gust.next - dtime
		if gust.next <= 0 then
			gust.phase, gust.t, gust.peak = "rise", 0, rand(0.6, 1.4)
			gust_rises()
		end
	elseif gust.phase == "rise" then
		gust.level = gust.peak * math.min(1, gust.t / 3)
		if gust.t >= 3 then gust.phase, gust.t = "hold", 0 end
	elseif gust.phase == "hold" then
		if gust.t >= rand(2, 6) then gust.phase, gust.t = "fall", 0 end
	elseif gust.phase == "fall" then
		gust.level = gust.peak * math.max(0, 1 - gust.t / 7)
		if gust.t >= 7 then
			gust.phase, gust.level, gust.next = "idle", 0, rand(15, 60)
		end
	end
end
core.register_globalstep(step_gust)

function A.wind_now()
	local t = core.get_gametime() or 0
	local swell = 0.75 + 0.15 * math.sin(t * 2 * math.pi / 180) + 0.1 * math.sin(t * 2 * math.pi / 47 + 1)
	return swell + 0.6 * gust.level
end

-- Where the wind blows: veers slowly over the day, backs a little in a gust
-- (a unit vector toward where it blows)
function A.wind_dir()
	if A.wind_override and A.wind_override.dir then return A.wind_override.dir end
	local t = core.get_gametime() or 0
	local a = 0.8 + 0.9 * math.sin(t * 2 * math.pi / 1300) + 0.25 * math.sin(t * 2 * math.pi / 210)
	a = a + 0.3 * gust.level
	return {x = math.cos(a), y = 0, z = math.sin(a)}
end

-- For tests and scripted moments: start a gust now
function A.gust_now(peak)
	gust.phase, gust.t, gust.peak = "rise", 0, peak or 1.2
	gust_rises()
end

--[[ The game, once per player ]]

-- Does the game play water and lava itself? Its module name differs between
-- games (env_sounds, vl_env_sounds, ...); its sound files give it away.
local function game_has_env_sounds()
	if core.get_modpath("vl_env_sounds") or core.get_modpath("env_sounds") then return true end
	for _, m in ipairs(core.get_modnames()) do
		if m ~= "afx_ambience" then
			local files = core.get_dir_list(core.get_modpath(m) .. "/sounds", false) or {}
			for _, f in ipairs(files) do
				if f:find("^env_sounds_water") or f:find("^env_sounds_lava") or
						f:find("^mcl_env_sounds") or f:find("^vl_env_sounds") then
					return true
				end
			end
		end
	end
	return false
end

local game_json, entities_json
core.register_on_mods_loaded(function()
	-- (the name and the seed: two worlds of the same name are still two)
	local world = (core.get_worldpath():match("([^/\\]+)$") or "") .. "-" ..
			tostring(core.get_mapgen_setting("seed") or "")
	local mods = core.get_modnames()
	table.sort(mods)
	game_json = core.write_json({
		id = (core.get_game_info and core.get_game_info().id) or "?",
		mods = mods,
		world = world,
		water_level = tonumber(core.get_mapgen_setting("water_level")) or 1,
		damage = core.settings:get_bool("enable_damage") ~= false,
		flags = {
			water_lava = game_has_env_sounds(),
			fire = core.get_modpath("mcl_fire") ~= nil or core.get_modpath("fire") ~= nil,
		},
	})
	-- Creatures: what kind (animal, monster), their voice, their size (a
	-- smaller one is young)
	local function sound_name(s)
		if type(s) == "table" then s = s.name end
		return type(s) == "string" and s or nil
	end
	local function sounds(s)
		if type(s) ~= "table" then return nil end
		local out = {random = sound_name(s.random), damage = sound_name(s.damage),
			base_pitch = tonumber(s.base_pitch)}
		return next(out) and out or nil
	end
	local ents = {}
	for name, def in pairs(core.registered_entities) do
		if name ~= "__builtin:item" and name ~= "__builtin:falling_node" then
			local props = def.initial_properties or def
			local vs = props.visual_size
			ents[name] = {
				type = type(def.type) == "string" and def.type or nil,
				passive = type(def.passive) == "boolean" and def.passive or nil,
				hostile = (def.hostile or def._hostile) and true or nil,
				sounds = sounds(def.sounds),
				sounds_child = sounds(def.sounds_child),
				visual_size = type(vs) == "table" and {x = tonumber(vs.x) or 1, y = tonumber(vs.y) or 1} or nil,
			}
		end
	end
	entities_json = core.write_json(ents)
	if #entities_json > 60000 then
		core.log("warning", "afx_ambience: creature table too large for one message, voices left out")
		for _, e in pairs(ents) do e.sounds_child = nil end
		entities_json = core.write_json(ents)
	end
end)

--[[ Every second, to every afx client: the weather and the place ]]

local sent = {} -- player name -> the game was sent

local function dimension(pos)
	local w = rawget(_G, "mcl_worlds")
	if w and w.pos_to_dimension then
		return w.pos_to_dimension(pos)
	end
	return "overworld"
end

-- The biomes around: samples on two rings, nearer ones weighing more, so
-- the client can blend a place's sound across a biome's edge. Sent when
-- they change (or every 10 s): "Name weight Name weight ..."
local BIOME_RADIUS = tonumber(core.settings:get("afx_ambience_biome_radius") or "") or 24
local biome_sent = {} -- player name -> {text, time}
local function biomes_around(pos)
	local w = {}
	local function add(p, weight)
		local bd = core.get_biome_data(p)
		local n = bd and core.get_biome_name(bd.biome)
		if n and n ~= "" then w[n] = (w[n] or 0) + weight end
	end
	add(pos, 1)
	for i = 0, 7 do
		local a = i * math.pi / 4
		for _, ring in ipairs({{0.5, 0.25}, {1, 0.12}}) do
			local r = BIOME_RADIUS * ring[1]
			add({x = pos.x + math.cos(a) * r, y = pos.y, z = pos.z + math.sin(a) * r}, ring[2])
		end
	end
	local total = 0
	for _, v in pairs(w) do total = total + v end
	local names = {}
	for n in pairs(w) do names[#names + 1] = n end
	table.sort(names)
	local parts = {}
	for _, n in ipairs(names) do
		-- (names go as words: anything else in one is left out)
		if n:match("^[%w_]+$") then parts[#parts + 1] = ("%s %.2f"):format(n, w[n] / total) end
	end
	return table.concat(parts, " ")
end

local timer = 0
core.register_globalstep(function(dtime)
	timer = timer + dtime
	if timer < 1 then return end
	timer = 0
	local d = A.wind_dir()
	for _, player in ipairs(core.get_connected_players()) do
		local name = player:get_player_name()
		-- an afx client says what it hears: those get the ambience's facts
		if player.get_acoustics and player:get_acoustics() then
			if not sent[name] then
				if player:send_afx_data("game", game_json) then
					if not player:send_afx_data("entities", entities_json) then
						core.log("warning", "afx_ambience: couldn't send the creature table to " .. name)
					end
					sent[name] = true
				end
			end
			local pos = player:get_pos()
			local bd = core.get_biome_data(pos)
			local w = {
				raining = A.rain_at(pos), snowing = A.snow_at(pos),
				wind_raw = A.wind_now(), wind_dir_x = d.x, wind_dir_z = d.z,
				wind_fixed = A.wind_override and A.wind_override.wind or nil,
				dimension = dimension(pos), day = core.get_day_count(),
				biome = bd and core.get_biome_name(bd.biome) or "",
				heat = bd and bd.heat or nil, humidity = bd and bd.humidity or nil,
			}
			player:set_afx_world(w)
			-- (A.biome_override: test scenarios set the biomes around)
			local text = A.biome_override or biomes_around(pos)
			local last = biome_sent[name]
			local now = core.get_us_time() / 1e6
			if text ~= "" and (not last or last.text ~= text or now - last.time > 10) then
				if player:send_afx_data("biomes", text) then
					biome_sent[name] = {text = text, time = now}
				end
			end
		end
	end
end)

core.register_on_leaveplayer(function(player)
	sent[player:get_player_name()] = nil
	biome_sent[player:get_player_name()] = nil
end)

--[[ Events ]]

local function to_all(channel, data)
	for _, player in ipairs(core.get_connected_players()) do
		player:send_afx_data(channel, data)
	end
end

-- A strike: the client plays the crack or the far roll and rattles windows.
-- `game_crack`: the game plays its own thunder at the strike.
function A.thunder_at(pos, game_crack)
	if not pos then return end
	to_all("strike", ("%.1f %.1f %.1f %d"):format(pos.x, pos.y, pos.z, game_crack and 1 or 0))
end

core.register_on_mods_loaded(function()
	local lightning = rawget(_G, "lightning")
	if lightning and lightning.register_on_strike then
		lightning.register_on_strike(function(pos, pos2) A.thunder_at(pos2 or pos, true) end)
	end
	-- explosions, wherever the game sets them off: windows rattle
	local function blast(pos, k)
		if pos then to_all("blast", ("%.1f %.1f %.1f %.2f"):format(pos.x, pos.y, pos.z, k)) end
	end
	local ex = rawget(_G, "mcl_explosions")
	if ex and ex.explode then
		local explode = ex.explode
		ex.explode = function(pos, strength, ...)
			blast(pos, math.min(1.5, (strength or 3) / 3))
			return explode(pos, strength, ...)
		end
	end
	local tnt = rawget(_G, "tnt")
	-- (where tnt goes through mcl_explosions, that wrap already hears it)
	if tnt and tnt.boom and not (ex and ex.explode) then
		local boom = tnt.boom
		tnt.boom = function(pos, def, ...)
			blast(pos, 1.2)
			return boom(pos, def, ...)
		end
	end
end)

--[[ Nodes dug and punched, by anyone or anything the server knows of (a
mod, a scripted scene): the digger's client hears what it shakes loose
(birds out of the leaves). Its own digs it also sees itself; it keeps one. ]]

local function node_event(what, pos, node, player)
	if player and player.is_player and player:is_player() then
		player:send_afx_data("node", ("%s %d %d %d %s"):format(what, pos.x, pos.y, pos.z, node.name))
	end
end
core.register_on_dignode(function(pos, oldnode, digger) node_event("dig", pos, oldnode, digger) end)
core.register_on_punchnode(function(pos, node, puncher) node_event("punch", pos, node, puncher) end)

--[[ Sounds from server content (content.lua): played by the client's
ambience, which has the sounds and their levels. Positional, to everyone in
earshot; a player without the afx client gets the plain sound. ]]

function A.play(name, params, ephemeral)
	params = params or {}
	local range = params.max_hear_distance or 32
	local msg = core.write_json({name = name, gain = params.gain, pitch = params.pitch,
		pos = params.pos, max_hear_distance = range})
	for _, player in ipairs(core.get_connected_players()) do
		local pname = player:get_player_name()
		if (not params.to_player or params.to_player == pname) and
				(not params.pos or vector.distance(player:get_pos(), params.pos) <= range) then
			if not player:send_afx_data("play", msg) then
				local p = table.copy(params)
				p.to_player = pname
				core.sound_play(name, p, true)
			end
		end
	end
end

-- Fast ticks for content (note stones under your feet)
local ticks, tick_timer = {}, 0
function A.register_tick(fn) ticks[#ticks + 1] = fn end
core.register_globalstep(function(dtime)
	tick_timer = tick_timer + dtime
	if tick_timer < 0.2 then return end
	for _, player in ipairs(core.get_connected_players()) do
		for _, fn in ipairs(ticks) do fn(player, tick_timer) end
	end
	tick_timer = 0
end)

--[[ For test scenarios ]]

-- Set a field of the client's per-player state ("next_timber", 0)
function A.poke(pname, field, value)
	local p = core.get_player_by_name(pname)
	if p then p:send_afx_data("poke", field .. " " .. tostring(value)) end
end

-- Run one of the client's commands ("/afx_report"), or send its formspec
-- fields ("afx:mixer", {...}); the client logs what happened
function A.client_chat(pname, line)
	local p = core.get_player_by_name(pname)
	if p then p:send_afx_data("chat", line) end
end
function A.client_fields(pname, formname, fields)
	local p = core.get_player_by_name(pname)
	if p then p:send_afx_data("fields", core.write_json({formname = formname, fields = fields})) end
end

-- What the client hears, roughly as its ambience reads it (for test logs)
A.lib = {}
function A.lib.sky_open(pos)
	local light = core.get_node_light(pos, 0.5)
	return light ~= nil and light >= 15
end
function A.where(ctx)
	local h = ctx.player.get_acoustics and ctx.player:get_acoustics()
	ctx.hearing = h
	h = h or {}
	ctx.under_sky = (h.sky or 0) > 0.5
	ctx.exposure = h.outdoor or 0
	ctx.through = h.wall or 0
	ctx.underground = not ctx.under_sky and (ctx.daylight or 0) <= 3 and ctx.exposure < 0.15 and
			(h.homey or 0) < 2 and (h.natural or 1) > 0.55
	ctx.indoors = not ctx.under_sky and not ctx.underground and (h.enclosure or 0) > 0.5 and
			ctx.exposure < 0.4
end

core.register_chatcommand("afx_rain", {
	params = "<0..1|off>",
	privs = {server = true},
	description = "Force rain intensity for afx_ambience (testing)",
	func = function(_, param)
		if param == "off" or param == "" then
			A.set_rain(nil)
			return true, "afx_ambience: rain follows the weather"
		end
		A.set_rain(tonumber(param) or 0.7)
		return true, "afx_ambience: rain forced to " .. param
	end,
})

dofile(core.get_modpath("afx_ambience") .. "/content.lua")
