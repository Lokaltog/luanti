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
	beds = setting_bool("afx_ambience_beds", true),
	use_biomes = setting_bool("afx_ambience_use_biomes", true),
	proximity = setting_bool("afx_ambience_proximity", true),
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

-- wanted: key -> {sound=, pos= (nil: a non-positional bed), gain=, fade=}
local function reconcile(pname, st, wanted)
	for key, w in pairs(wanted) do
		if w.gain < 0.01 then wanted[key] = nil end
	end
	for key, e in pairs(st.emitters) do
		local w = wanted[key]
		if not w then
			core.sound_fade(e.handle, e.fade, 0)
			st.emitters[key] = nil
		elseif math.abs(w.gain - e.gain) > 0.02 then
			core.sound_fade(e.handle, w.fade or FADE, w.gain)
			e.gain = w.gain
		end
	end
	for key, w in pairs(wanted) do
		if not st.emitters[key] then
			local handle = core.sound_play(w.sound, {
				pos = w.pos, to_player = pname, loop = true, gain = 0.01,
				max_hear_distance = w.pos and 32 or nil,
				pitch = w.pos and rand(0.95, 1.05) or 1,
				start_time = rand(0, 20),
			})
			if handle then
				core.sound_fade(handle, w.fade or FADE, w.gain)
				st.emitters[key] = {handle = handle, gain = w.gain, fade = w.fade or FADE}
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

--[[ Beds: what the place sounds like from afar (outdoor bus: the engine
     muffles them indoors and underground and steers them through openings) ]]

local function count(ppos, r, dy0, dy1, names)
	return #core.find_nodes_in_area(vector.offset(ppos, -r, dy0, -r),
			vector.offset(ppos, r, dy1, r), names)
end

local function has(name, words)
	for _, w in ipairs(words) do
		if name:find(w, 1, true) then return true end
	end
	return false
end

-- What kind of place, 0..1 each, from the biome name and the nodes around.
local function place(ppos)
	local p = {forest = 0, meadow = 0, jungle = 0, coast = 0, cold = 0, dry = 0, swamp = 0}
	local bd = CFG.use_biomes and core.get_biome_data and core.get_biome_data(ppos)
	local bname = bd and core.get_biome_name(bd.biome) or ""
	bname = bname:lower()
	if has(bname, {"jungle", "bamboo"}) then p.jungle = 1
	elseif has(bname, {"forest", "taiga", "birch", "grove", "roofed"}) then p.forest = 0.7
	elseif has(bname, {"desert", "mesa", "badlands", "savanna"}) then p.dry = 1
	elseif has(bname, {"ocean", "beach", "shore", "coast"}) then p.coast = 0.7
	elseif has(bname, {"ice", "snow", "frozen", "cold"}) then p.cold = 1
	elseif has(bname, {"swamp", "mangrove", "marsh"}) then p.swamp = 1
	else p.meadow = 0.8 end

	-- Local evidence beats the biome map: trees around make it a forest,
	-- open water makes it a shore, flowers make it a meadow.
	local leaves = count(ppos, 14, -4, 16, {"group:leaves"})
	p.forest = math.max(p.forest, math.min(1, leaves / 120))
	local water = count(ppos, 14, -6, 1, {"group:water"})
	p.coast = math.max(p.coast, math.min(1, (water - 40) / 300))
	local flowers = count(ppos, 10, -3, 3, {"group:flower"})
	p.meadow = math.max(p.meadow, math.min(1, flowers / 12))
	return p
end

local function bed_targets(ppos, wanted, raining)
	local tod = core.get_timeofday()
	local day = math.max(0, math.min(1, (0.32 - math.abs(tod - 0.5)) / 0.05))
	local night = 1 - day
	local p = place(ppos)
	-- Deep underground nothing of this would be audible: free the voices
	if ppos.y < -40 and not sky_open(vector.round(vector.offset(ppos, 0, 1.6, 0))) then
		return
	end
	-- Higher and more exposed is windier; floating islands howl
	local height = math.max(0, math.min(1, (ppos.y - 40) / 60))
	local calm_birds = raining > 0 and 0 or 1

	wanted.b_wind = {sound = "afx_out_wind", gain = 0.35 * (1 - 0.6 * p.forest) + 0.2 * height, fade = 0.2}
	wanted.b_howl = {sound = "afx_out_wind_howl", gain = 0.7 * height + 0.3 * p.cold, fade = 0.2}
	wanted.b_trees = {sound = "afx_out_wind_trees", gain = 0.6 * p.forest, fade = 0.2}
	wanted.b_birds = {sound = "afx_out_birds_far",
		gain = day * calm_birds * (0.25 * p.meadow + 0.35 * p.forest) * (1 - height), fade = 0.15}
	wanted.b_crickets = {sound = "afx_out_crickets",
		gain = night * calm_birds * 0.45 * math.max(p.meadow, p.forest, p.dry), fade = 0.1}
	wanted.b_frogs = {sound = "afx_out_frogs",
		gain = night * 0.4 * math.max(p.swamp, p.coast * 0.5), fade = 0.1}
	wanted.b_jungle = {sound = "afx_out_jungle", gain = 0.6 * p.jungle, fade = 0.2}
	wanted.b_waves = {sound = "afx_out_waves", gain = 0.7 * p.coast, fade = 0.2}
end

--[[ Proximity: things that sound where they are ]]

local function proximity_emitters(ppos, wanted)
	local r = CFG.radius + 2
	local lo, hi = vector.subtract(ppos, r), vector.add(ppos, r)
	local day = math.abs(core.get_timeofday() - 0.5) < 0.27

	-- Running water: flowing nodes; a waterfall where flow falls onto flow
	local flowing, falls = {}, {}
	for _, p in ipairs(core.find_nodes_in_area(lo, hi, {"group:water"})) do
		local name = core.get_node(p).name
		if name:find("flowing") then
			local below = core.get_node(vector.offset(p, 0, -1, 0)).name
			if below:find("flowing") or below == "air" then
				falls[#falls + 1] = p
			else
				flowing[#flowing + 1] = p
			end
		end
	end
	for _, c in ipairs(nearest(clusters(falls), ppos, 2)) do
		if c.n >= 3 then
			wanted["wf" .. c.key] = {sound = "afx_waterfall", pos = c.pos, gain = math.min(0.9, 0.2 + 0.05 * c.n)}
		end
	end
	for _, c in ipairs(nearest(clusters(flowing), ppos, 3)) do
		if c.n >= 2 then
			wanted["rv" .. c.key] = {sound = "afx_river", pos = c.pos, gain = math.min(0.6, 0.1 + 0.04 * c.n)}
		end
	end

	-- Insects over flowers, by day
	if day then
		local flowers = core.find_nodes_in_area(lo, hi, {"group:flower"})
		for _, c in ipairs(nearest(clusters(flowers), ppos, 2)) do
			if c.n >= 3 then
				wanted["fl" .. c.key] = {sound = "afx_insects", pos = c.pos, gain = math.min(0.35, 0.05 * c.n)}
			end
		end
	end

	-- Bees, fires, lit furnaces and campfires
	local hot, bees = {}, {}
	for _, p in ipairs(core.find_nodes_in_area(lo, hi, {"group:fire", "group:bee_nest",
			"group:beehive", "group:furnace", "group:campfire"})) do
		local name = core.get_node(p).name
		if has(name, {"bee"}) then
			bees[#bees + 1] = p
		elseif not has(name, {"furnace"}) or has(name, {"active", "lit"}) then
			hot[#hot + 1] = p
		end
	end
	for _, c in ipairs(nearest(clusters(hot), ppos, 2)) do
		wanted["fi" .. c.key] = {sound = "afx_fire", pos = c.pos, gain = math.min(0.6, 0.25 + 0.05 * c.n)}
	end
	for _, c in ipairs(nearest(clusters(bees), ppos, 2)) do
		wanted["be" .. c.key] = {sound = "afx_bees", pos = c.pos, gain = 0.5}
	end
end

--[[ Under water ]]

local function underwater_bed(player, wanted)
	local head = vector.round(vector.offset(player:get_pos(), 0, 1.6, 0))
	if core.get_item_group(core.get_node(head).name, "water") > 0 then
		wanted.u_water = {sound = "afx_in_underwater", gain = 0.7, fade = 2.0}
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
	-- Only where daylight never reaches (a cave, not a cellar with a door)
	local day_light = core.get_node_light(ear, 0.5)
	if not day_light or day_light > 3 then return end
	-- A ceiling: solid stone with air under it, within a few nodes up
	local ceil = core.find_nodes_in_area(vector.offset(ear, -8, 2, -8),
			vector.offset(ear, 8, 16, 8), {"group:stone", "group:pickaxey", "group:cracky"})
	local under = {}
	for _ = 1, 24 do
		if #ceil == 0 then break end
		local p = pick(ceil)
		local below = core.get_node_or_nil(vector.offset(p, 0, -1, 0))
		local def = core.registered_nodes[core.get_node(p).name]
		if below and below.name == "air" and def and (def.light_source or 0) == 0 then
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
		if CFG.beds then
			bed_targets(ppos, wanted, rain)
		end
		if CFG.proximity then
			proximity_emitters(ppos, wanted)
		end
		underwater_bed(player, wanted)
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
