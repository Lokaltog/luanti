-- afx_ambience: the content layer of the afx fork's environmental audio.
--
-- The engine (afx fork) does the acoustics: reverb, echo, occlusion and
-- outdoor muffling from the blocks around the listener. This mod decides
-- *what* plays and *where*, from the world: rain drumming on the actual
-- roof above you and pattering on nearby windows, birds calling from real
-- leaves now and then, lava bubbling where the lava is, drips falling from
-- cave ceilings. Every sound is a real positional source, so the engine
-- occludes and reverberates it like anything else.
--
-- Works on a stock engine too (just without the acoustics). Weather comes
-- from Mineclonia/VoxeLibre's mcl_weather when present.

afx_ambience = {}

local function setting_bool(name, default)
	local v = core.settings:get_bool(name)
	if v == nil then return default end
	return v
end

local function setting_num(name, default)
	return tonumber(core.settings:get(name) or "") or default
end

local CFG = {
	rain_surfaces = setting_bool("afx_ambience_rain_surfaces", true),
	birds = setting_bool("afx_ambience_birds", true),
	bird_interval = setting_num("afx_ambience_bird_interval", 25), -- mean seconds
	cave = setting_bool("afx_ambience_cave", true),
	lava = setting_bool("afx_ambience_lava", true),
	radius = setting_num("afx_ambience_radius", 10),
	debug = setting_bool("afx_ambience_debug", false),
}

local function log(msg)
	if CFG.debug then
		core.log("action", "[afx_ambience] " .. msg)
	end
end

local rng = PcgRandom(os.time())
local function rand(a, b) return a + (b - a) * (rng:next(0, 1000000) / 1000000) end
local function pick(t) return t[rng:next(1, #t)] end

--[[ Weather ]]

-- Rain intensity 0..1 at a position. Forced with afx_ambience.set_rain().
local forced_rain
function afx_ambience.set_rain(intensity)
	forced_rain = intensity
end

function afx_ambience.rain_at(pos)
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
			return 0 -- dry or snowy biome
		end
	end
	return state == "thunder" and 1 or 0.7
end

--[[ Exposure: is this air open to the sky? (sunlight 15 at noon) ]]

local function sky_open(pos)
	local light = core.get_node_light(pos, 0.5)
	return light ~= nil and light >= 15
end

local NEIGHBORS = {
	{x = 1, y = 0, z = 0}, {x = -1, y = 0, z = 0}, {x = 0, y = 1, z = 0},
	{x = 0, y = 0, z = 1}, {x = 0, y = 0, z = -1},
}

-- A surface gets rained on when a neighbour is air under open sky.
local function rained_on(pos)
	for _, d in ipairs(NEIGHBORS) do
		local p = vector.add(pos, d)
		local node = core.get_node_or_nil(p)
		if node and node.name == "air" and sky_open(p) then
			return true
		end
	end
	return false
end

--[[ Surface material -> rain sound ]]

local function rain_sound_for(name)
	local def = core.registered_nodes[name]
	if not def then return nil end
	local g = def.groups or {}
	if (g.glass or 0) > 0 or (g.pane or 0) > 0 or name:find("glass") then
		return "afx_rain_glass", 0.6
	end
	if (g.leaves or 0) > 0 or name:find("leaves") then
		return "afx_rain_leaves", 0.6
	end
	if name:find("iron") or name:find("copper") or name:find("metal") then
		return "afx_rain_metal", 0.45
	end
	if (g.wood or 0) > 0 or (g.axey or 0) > 0 or (g.choppy or 0) > 0 or (g.tree or 0) > 0 then
		return "afx_rain_wood", 0.45
	end
	-- stone, earth: a dull roof; the wood drumming, quieter
	return "afx_rain_wood", 0.25
end

--[[ Emitters: positional loops reconciled every scan, faded in and out ]]

local players = {}

local function player_state(name)
	local st = players[name]
	if not st then
		local now = core.get_us_time() / 1e6
		st = {emitters = {}, next_bird = now + rand(4, CFG.bird_interval), next_drip = now + rand(1, 4)}
		players[name] = st
	end
	return st
end

local FADE = 0.5 -- gain per second

local function reconcile(pname, st, wanted)
	-- wanted: key -> {sound=, pos=, gain=}
	for key, e in pairs(st.emitters) do
		local w = wanted[key]
		if not w then
			core.sound_fade(e.handle, FADE, 0)
			st.emitters[key] = nil
		elseif math.abs(w.gain - e.gain) > 0.05 then
			core.sound_fade(e.handle, FADE, w.gain)
			e.gain = w.gain
		end
	end
	for key, w in pairs(wanted) do
		if not st.emitters[key] then
			local handle = core.sound_play(w.sound, {
				pos = w.pos, to_player = pname, loop = true, gain = 0.01,
				max_hear_distance = 32, pitch = rand(0.95, 1.05),
				start_time = rand(0, 20),
			})
			if handle then
				core.sound_fade(handle, FADE, w.gain)
				st.emitters[key] = {handle = handle, gain = w.gain}
			end
		end
	end
end

-- Group positions into 4-node cells; one emitter per cell at its centroid.
local function clusters(positions, weight_of)
	local cells = {}
	for _, p in ipairs(positions) do
		local k = math.floor(p.x / 4) .. "," .. math.floor(p.y / 4) .. "," .. math.floor(p.z / 4)
		local c = cells[k]
		if not c then
			c = {sum = vector.zero(), n = 0, w = 0, key = k}
			cells[k] = c
		end
		c.sum = vector.add(c.sum, p)
		c.n = c.n + 1
		c.w = c.w + (weight_of and weight_of(p) or 1)
	end
	local out = {}
	for _, c in pairs(cells) do
		c.pos = vector.divide(c.sum, c.n)
		out[#out + 1] = c
	end
	return out
end

local function nearest(list, pos, n)
	table.sort(list, function(a, b)
		return vector.distance(a.pos, pos) < vector.distance(b.pos, pos)
	end)
	local out = {}
	for i = 1, math.min(n, #list) do out[i] = list[i] end
	return out
end

--[[ Rain on surfaces ]]

-- Surface layers sit a node or two from the ear and positional sources are
-- loud up close; keep them under the rain bed itself.
local SURFACE_GAIN = 0.25

local function rain_emitters(ppos, intensity, wanted)
	intensity = intensity * SURFACE_GAIN
	local r = CFG.radius
	local ear = vector.offset(ppos, 0, 1.6, 0)
	local under_sky = sky_open(vector.round(ear))

	-- Windows and glass roofs within reach
	local glass = core.find_nodes_in_area(vector.subtract(ppos, r), vector.add(ppos, r),
			{"group:glass", "group:pane"})
	local exposed = {}
	for _, p in ipairs(glass) do
		if rained_on(p) then exposed[#exposed + 1] = p end
	end
	for _, c in ipairs(nearest(clusters(exposed), ear, 4)) do
		local g = math.min(0.7, 0.15 + 0.08 * c.n) * intensity * (under_sky and 0.4 or 1)
		wanted["g" .. c.key] = {sound = "afx_rain_glass", pos = c.pos, gain = g}
	end

	-- The roof right above you; under open sky only a leaf canopy counts
	do
		local roof = {}
		local base = vector.round(ear)
		for dx = -3, 3, 2 do
			for dz = -3, 3, 2 do
				for dy = 1, 12 do
					local p = vector.offset(base, dx, dy, dz)
					local node = core.get_node_or_nil(p)
					if not node then break end
					local def = core.registered_nodes[node.name]
					if def and def.walkable then
						local leafy = core.get_item_group(node.name, "leaves") > 0
						if (leafy or not under_sky) and sky_open(vector.offset(p, 0, 1, 0)) then
							roof[#roof + 1] = {pos = p, name = node.name}
						end
						break
					end
				end
			end
		end
		local by_sound = {}
		for _, rp in ipairs(roof) do
			local snd, g = rain_sound_for(rp.name)
			if snd then
				local b = by_sound[snd] or {sum = vector.zero(), n = 0, g = g}
				b.sum = vector.add(b.sum, rp.pos)
				b.n = b.n + 1
				by_sound[snd] = b
			end
		end
		for snd, b in pairs(by_sound) do
			local pos = vector.divide(b.sum, b.n)
			local cover = math.min(1, b.n / 8)
			wanted["r" .. snd] = {sound = snd, pos = pos, gain = b.g * cover * intensity}
		end
	end
end

--[[ Lava ]]

local function lava_emitters(ppos, wanted)
	local r = CFG.radius + 4
	local lava = core.find_nodes_in_area(vector.subtract(ppos, r), vector.add(ppos, r),
			{"group:lava"})
	if #lava == 0 then return end
	local surface = {}
	for _, p in ipairs(lava) do
		local above = core.get_node_or_nil(vector.offset(p, 0, 1, 0))
		if above and above.name == "air" then surface[#surface + 1] = p end
	end
	for _, c in ipairs(nearest(clusters(surface), ppos, 3)) do
		local g = math.min(1, 0.3 + 0.05 * c.n)
		wanted["l" .. c.key] = {sound = "afx_lava_bubble", pos = c.pos, gain = g}
		wanted["L" .. c.key] = {sound = "afx_lava", pos = c.pos, gain = g * 0.6}
	end
end

--[[ One-shots: birds and drips ]]

local DAY_BIRDS = {"afx_bird_robin", "afx_bird_cardinal", "afx_bird_lark", "afx_bird_robin",
	"afx_bird_cardinal", "afx_bird_crow"}
local NIGHT_BIRDS = {"afx_bird_owl"}

local function maybe_bird(pname, st, ppos, now, raining)
	if not CFG.birds or now < st.next_bird then return end
	local tod = core.get_timeofday()
	local night = tod < 0.2 or tod > 0.8
	-- Exponential gaps: mostly quiet, sometimes two calls close together
	local mean = CFG.bird_interval * (night and 2.5 or 1)
	st.next_bird = now - math.log(math.max(1e-3, rand(0, 1))) * mean + 3
	if raining > 0 then return end
	local leaves = core.find_nodes_in_area(vector.offset(ppos, -20, -2, -20),
			vector.offset(ppos, 20, 14, 20), {"group:leaves"})
	if #leaves < 6 then return end -- no trees, no birds
	local spot = pick(leaves)
	if vector.distance(spot, ppos) < 5 then spot = pick(leaves) end
	local sound = night and pick(NIGHT_BIRDS) or pick(DAY_BIRDS)
	core.sound_play(sound, {pos = spot, to_player = pname, gain = rand(0.35, 0.8),
		pitch = rand(0.92, 1.08), max_hear_distance = 40}, true)
	log(pname .. ": " .. sound .. " at " .. core.pos_to_string(spot))
end

local function maybe_drip(pname, st, ppos, now)
	if not CFG.cave or now < st.next_drip then return end
	st.next_drip = now + rand(1.2, 6)
	local ear = vector.round(vector.offset(ppos, 0, 1.6, 0))
	if sky_open(ear) or ppos.y > 20 then return end
	-- A ceiling: solid stone with air under it, within a few nodes up
	local ceil = core.find_nodes_in_area(vector.offset(ear, -8, 2, -8),
			vector.offset(ear, 8, 16, 8), {"group:stone", "group:pickaxey", "group:cracky"})
	local under = {}
	for _ = 1, 24 do
		if #ceil == 0 then break end
		local p = pick(ceil)
		local below = core.get_node_or_nil(vector.offset(p, 0, -1, 0))
		if below and below.name == "air" then
			under[#under + 1] = vector.offset(p, 0, -0.6, 0)
			break
		end
	end
	if #under == 0 then return end
	core.sound_play("afx_drip", {pos = under[1], to_player = pname,
		gain = rand(0.2, 0.6), pitch = rand(0.85, 1.15), max_hear_distance = 24}, true)
end

--[[ Main loop ]]

local timer = 0
local SCAN = 1.0

core.register_globalstep(function(dtime)
	timer = timer + dtime
	if timer < SCAN then return end
	timer = 0
	local now = core.get_us_time() / 1e6
	for _, player in ipairs(core.get_connected_players()) do
		local pname = player:get_player_name()
		local st = player_state(pname)
		local ppos = player:get_pos()
		local rain = afx_ambience.rain_at(ppos)
		local wanted = {}
		if CFG.rain_surfaces and rain > 0 then
			rain_emitters(ppos, rain, wanted)
		end
		if CFG.lava then
			lava_emitters(ppos, wanted)
		end
		reconcile(pname, st, wanted)
		maybe_bird(pname, st, ppos, now, rain)
		maybe_drip(pname, st, ppos, now)
	end
end)

core.register_on_leaveplayer(function(player)
	players[player:get_player_name()] = nil
end)

core.register_chatcommand("afx_rain", {
	params = "<0..1|off>",
	privs = {server = true},
	description = "Force rain intensity for afx_ambience (testing)",
	func = function(_, param)
		if param == "off" or param == "" then
			afx_ambience.set_rain(nil)
			return true, "afx_ambience: rain follows the weather"
		end
		afx_ambience.set_rain(tonumber(param) or 0.7)
		return true, "afx_ambience: rain forced to " .. param
	end,
})
