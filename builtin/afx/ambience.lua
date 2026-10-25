-- The ambience's core: layers, emitters reconciled every second, where you
-- are, the mix (levels and per-layer volumes), rain on surfaces, lava,
-- the beds, things nearby, birds and drips. See init.lua.

local afx = core.afx

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

--[[ Per-layer volumes (the /afx mixer); live, saved as settings ]]

-- {key, short label, what's in it (tooltip)}
local LAYERS = {
	{"beds", "Background", "Wind, birds far off, crickets, waves, the jungle"},
	{"rain", "Rain on things", "Rain on roofs and windows, gutters, first drops, the receding tail"},
	{"birds", "Birds", "Birds in the trees, flushed from leaves"},
	{"water", "Water", "Shores, rivers, waterfalls, splashes, frogs, swimming"},
	{"places", "Places", "Fires, furnaces, drips, creaks, chains, clocks, things landing"},
	{"body", "Your body", "Rushing air, brushing plants, held items"},
	{"thunder", "Thunder", "Thunder near and far, windows rattling"},
	{"chimes", "Chimes & bells", "Wind chimes, bells"},
	{"heart", "Heartbeat", "Heartbeat when hurt or something's close in the dark"},
	{"drafts", "Drafts", "Gaps in walls whistling in the wind"},
	{"music", "Music", "Note stones, music boxes, discovery chimes"},
}
afx_ambience.LAYERS = LAYERS

local VOL = {}
local function read_vol(k)
	VOL[k] = setting_num("afx_ambience_vol_" .. k, 1)
end
for _, l in ipairs(LAYERS) do read_vol(l[1]) end

function afx_ambience.set_volume(k, v)
	VOL[k] = v
	core.settings:set("afx_ambience_vol_" .. k, string.format("%.2f", v))
end
function afx_ambience.volume(k) return VOL[k] or 1 end

local CATEGORY = {
	{"^afx_out_", "beds"}, {"^afx_in_", "beds"}, {"^afx_gust", "beds"}, {"^afx_cricket", "beds"},
	{"^afx_ice_", "water"}, {"^afx_cloth", "body"}, {"^afx_dry_breath", "body"},
	{"^afx_rain_", "rain"}, {"^afx_raindrop", "rain"}, {"^afx_gutter", "rain"}, {"^afx_drip_after", "rain"},
	{"^afx_bird_", "birds"},
	{"^afx_shore", "water"}, {"^afx_river", "water"}, {"^afx_waterfall", "water"},
	{"^afx_plop", "water"}, {"^afx_bubbles", "water"}, {"^afx_frog", "water"},
	{"^afx_splash", "water"}, {"^afx_swim", "water"}, {"^afx_dry_gasp", "water"},
	{"^afx_thunder", "thunder"},
	{"^afx_bowl", "chimes"}, {"^mcl_bells", "chimes"},
	{"^afx_dry_heart", "heart"},
	{"^afx_draft", "drafts"},
	{"^afx_note_", "music"}, {"^afx_dry_score_", "music"},
	{"^afx_dry_rush", "body"}, {"^afx_leaf_rustle", "body"}, {"^afx_dry_tick", "body"},
	{"^afx_dry_seashell", "body"}, {"^afx_whizz", "body"},
}
local cat_cache = {}
function afx_ambience.category(name)
	local c = cat_cache[name]
	if c == nil then
		c = "places"
		for _, m in ipairs(CATEGORY) do
			if name:find(m[1]) then c = m[2] break end
		end
		cat_cache[name] = c
	end
	return c
end

-- Per place, per player: subtle at home, cinematic in caves and storms
-- (set in where(); her music and the heartbeat aren't scaled)
local PLACE = {}
local UNSCALED = {music = true, heart = true, thunder = true}
local function place_scale(cat, pname)
	if UNSCALED[cat] or not pname then return 1 end
	return PLACE[pname] or 1
end

-- The mix: each sound group's level (dB) on top of its files, which the
-- sound pipeline keeps at their category's reference loudness
-- (util/afx/sound_pipeline.py). Tune levels in levels.lua, not in the code.
local LEVELS = dofile(core.get_builtin_path() .. "afx" .. DIR_DELIM .. "levels.lua")
local level_cache = {}
local function level_of(name)
	local l = level_cache[name]
	if not l then
		local db = LEVELS[name]
		if not db then
			-- a group's variants share its level ("afx_drip" for afx_drip.3)
			db = LEVELS[(name:gsub("%.%d+$", ""))] or 0
		end
		l = 10 ^ (db / 20)
		level_cache[name] = l
	end
	return l
end
afx_ambience.level_of = level_of

-- Every sound this mod plays goes through here, scaled by its level, its
-- layer's volume and the place
function afx_ambience.play(name, params, ephemeral)
	local cat = afx_ambience.category(name)
	params = params or {}
	local v = (VOL[cat] or 1) * place_scale(cat, params.to_player)
	if v <= 0.001 then return nil end
	params.gain = (params.gain or 1) * v * level_of(name)
	return core.sound_play(name, params, ephemeral)
end
function afx_ambience.gain_for(name, gain, pname)
	local cat = afx_ambience.category(name)
	return gain * (VOL[cat] or 1) * place_scale(cat, pname) * level_of(name)
end

local function log(msg)
	if CFG.debug then
		core.log("action", "[afx_ambience] " .. msg)
	end
end

local rng = PcgRandom(os.time())
local function rand(a, b) return a + (b - a) * (rng:next(0, 1000000) / 1000000) end
local function pick(t) return t[rng:next(1, #t)] end

--[[ Weather: from the server (afx_ambience's bridge), at the player ]]

function afx_ambience.rain_at()
	return tonumber(afx.world.raining) or 0
end

-- Snowfall 0..1 here
function afx_ambience.snow_at()
	return tonumber(afx.world.snowing) or 0
end

--[[ What the game already plays: don't double it ]]

-- (the server says: its water and lava sounds, its fire sounds)
local function game_has(flag) return afx.game.flags[flag] == true end

-- Fires, lit furnaces and campfires, beehives: by name, since games differ
-- in their groups
local HOT_NODES, BEE_NODES, FURNACE_NODES = {}, {}, {}
core.register_on_mods_loaded(function()
	for name, def in pairs(core.registered_nodes) do
		local lit = name:find("_active") or name:find("_lit") or name:find(":lit_")
		if (name:find("furnace") or name:find("smoker")) and lit then
			FURNACE_NODES[#FURNACE_NODES + 1] = name
		elseif name:find("campfire") and lit then
			HOT_NODES[#HOT_NODES + 1] = name
		elseif name:find("campfire") and not name:find("_off") and not name:find("unlit") and
				(def.light_source or 0) > 0 then
			HOT_NODES[#HOT_NODES + 1] = name
		elseif name:find("bee_nest") or name:find("beehive") then
			BEE_NODES[#BEE_NODES + 1] = name
		end
	end
end)
-- fire crackles on its own in games with a fire mod
local function hot_nodes()
	if game_has("fire") then return HOT_NODES end
	local t = {"group:fire"}
	for _, n in ipairs(HOT_NODES) do t[#t + 1] = n end
	return t
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

-- wanted: key -> {sound=, pos= or object= (neither: a non-positional bed),
--                 gain=, fade=, range=}
local function reconcile(pname, st, wanted)
	for key, w in pairs(wanted) do
		w.gain = afx_ambience.gain_for(w.sound, w.gain, pname)
		if w.gain < 0.01 then wanted[key] = nil end
	end
	for key, e in pairs(st.emitters) do
		local w = wanted[key]
		-- the same key somewhere else now (roof rain over another room):
		-- let the old one fade where it was and start again over there
		if w and w.pos and e.pos and vector.distance(w.pos, e.pos) > 1.5 then
			core.sound_fade(e.handle, e.fade, 0)
			st.emitters[key] = nil
		elseif not w then
			core.sound_fade(e.handle, e.fade, 0)
			st.emitters[key] = nil
		elseif math.abs(w.gain - e.gain) > 0.02 then
			core.sound_fade(e.handle, w.fade or FADE, w.gain)
			e.gain = w.gain
		end
	end
	for key, w in pairs(wanted) do
		if not st.emitters[key] then
			local positional = w.pos or w.object
			local handle = core.sound_play(w.sound, {
				pos = w.pos, object = w.object, to_player = pname, loop = true, gain = 0.01,
				max_hear_distance = positional and (w.range or 32) or nil,
				pitch = w.pitch or (positional and rand(0.95, 1.05) or 1),
				start_time = rand(0, 20),
			})
			if handle then
				core.sound_fade(handle, w.fade or FADE, w.gain)
				st.emitters[key] = {handle = handle, gain = w.gain, fade = w.fade or FADE, pos = w.pos}
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

local function rain_emitters(ctx, intensity, wanted)
	local ppos = ctx.ppos
	intensity = intensity * SURFACE_GAIN
	local r = CFG.radius
	local ear = vector.offset(ppos, 0, 1.6, 0)
	-- out in it (a jungle floor under a canopy counts: the open path is wide)
	local under_sky = ctx.under_sky or ctx.exposure > 0.4

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
						-- climb through a roof up to 4 thick to its top
						local top, tname = p, node.name
						for _ = 1, 4 do
							local q = vector.offset(top, 0, 1, 0)
							local qn = core.get_node_or_nil(q)
							local qd = qn and core.registered_nodes[qn.name]
							if not (qd and qd.walkable) then break end
							top, tname = q, qn.name
						end
						local leafy = core.get_item_group(tname, "leaves") > 0
						if (leafy or not under_sky) and sky_open(vector.offset(top, 0, 1, 0)) then
							-- heard at the ceiling, where it drums: indoors that's
							-- the ceiling's own material (planks under a leafy top
							-- still drum like planks)
							roof[#roof + 1] = {pos = p, name = under_sky and tname or node.name}
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

local st_for_place -- the player place() is looking around for

-- Sea level, for telling the sea from a cave lake or a pond up a hill

-- What kind of place, 0..1 each, from the biome name and the nodes around.
local function place(ppos, under_sky)
	-- (called above ground only: bed_targets returns early underground)
	local p = {forest = 0, meadow = 0, jungle = 0, coast = 0, cold = 0, dry = 0, swamp = 0}
	local bd = CFG.use_biomes and core.get_biome_data and core.get_biome_data(ppos)
	local bname = bd and core.get_biome_name(bd.biome) or ""
	bname = bname:lower()
	-- Mineclonia/VoxeLibre name the layers of a biome: Forest_ocean is the
	-- sea bed off a forest (and the rock under it), Forest_underground the
	-- caves below. Only the surface is the place you're in.
	local WATER_LEVEL = tonumber(afx.game.water_level) or 1
	local near_sea = under_sky and ppos.y >= WATER_LEVEL - 3 and ppos.y <= WATER_LEVEL + 12
	local layer = bname:match("_(deep_ocean)$") or bname:match("_(ocean)$") or
			bname:match("_(underground)$") or bname:match("_(beach)$") or bname:match("_(shore)$")
	if layer then bname = bname:sub(1, #bname - #layer - 1) end
	if (layer == "beach" or layer == "shore") and near_sea then
		p.coast = 0.7
	elseif (layer == "ocean" or layer == "deep_ocean") and near_sea then
		p.coast = 0.8
	elseif layer == "underground" or layer == "ocean" or layer == "deep_ocean" then
		-- below the surface: no birds' biome to speak of
		bname = ""
	end
	if bname == "" then
		p.meadow = 0
	elseif has(bname, {"jungle", "bamboo"}) then p.jungle = 1
	elseif has(bname, {"forest", "taiga", "birch", "grove", "roofed"}) then p.forest = 0.7
	elseif has(bname, {"desert", "mesa", "badlands", "savanna"}) then p.dry = 1
	elseif has(bname, {"ocean", "beach", "shore", "coast"}) then
		if near_sea then p.coast = math.max(p.coast, 0.7) end
	elseif has(bname, {"ice", "snow", "frozen", "cold"}) then p.cold = 1
	elseif has(bname, {"swamp", "mangrove", "marsh"}) then p.swamp = 1
	else p.meadow = 0.8 end

	-- Local evidence beats the biome map: trees around make it a forest,
	-- open water makes it a shore, flowers make it a meadow.
	local leaves = count(ppos, 14, -4, 16, {"group:leaves"})
	if st_for_place then st_for_place.trees = leaves end
	p.forest = math.max(p.forest, math.min(1, leaves / 120))
	-- The sea: a wide sheet of water at sea level, open to the sky. A cave
	-- lake, a river or a pond on a hill is not the coast.
	if near_sea then
		local r = 22
		local surface = #core.find_nodes_in_area(
				{x = ppos.x - r, y = WATER_LEVEL - 1, z = ppos.z - r},
				{x = ppos.x + r, y = WATER_LEVEL - 1, z = ppos.z + r}, {"group:water"})
		p.coast = math.max(p.coast, math.min(1, (surface - 250) / 600))
		if surface < 60 then p.coast = 0 end -- the biome map says sea, the eye says no
	else
		p.coast = 0
	end
	local flowers = count(ppos, 10, -3, 3, {"group:flower"})
	p.meadow = math.max(p.meadow, math.min(1, flowers / 12))
	return p
end

local function bed_targets(ctx, wanted)
	local ppos, raining, snowing = ctx.ppos, ctx.rain, ctx.snow
	if ctx.dimension ~= "overworld" then return end
	local tod = core.get_timeofday()
	local day = math.max(0, math.min(1, (0.32 - math.abs(tod - 0.5)) / 0.05))
	local night = 1 - day
	-- Where nothing from outside would be heard, free the voices
	if ctx.hearing then
		-- the engine says nothing from outside reaches this spot
		if ctx.exposure < 0.005 and ctx.through < 0.003 then return end
	elseif ctx.underground then
		return
	end
	st_for_place = ctx.st
	local p = place(ppos, ctx.under_sky or ctx.exposure > 0.3)
	st_for_place = nil
	ctx.place = p
	ctx.st.last_place = p
	-- Higher and more exposed is windier; floating islands howl
	local height = math.max(0, math.min(1, (ppos.y - 40) / 60))
	local calm_birds = (raining > 0 or snowing > 0) and 0 or (ctx.birds_back or 1)
	-- Snow hushes the world: a fresh blanket on the ground, or snow falling
	local snow_cover = math.min(1, count(ppos, 8, -3, 1, {"group:snow_cover", "group:snow",
		"group:snow_layer", "group:top_snow"}) / 60)
	local hush = 1 - 0.55 * math.max(snowing, snow_cover)

	-- ctx.wind: gusts over minutes x exposure (ridges, peaks), see weather.lua
	local wind = ctx.wind or 1
	wanted.b_wind = {sound = "afx_out_wind",
		gain = hush * wind * (0.35 * (1 - 0.6 * p.forest) + 0.2 * height), fade = 0.25}
	wanted.b_howl = {sound = "afx_out_wind_howl",
		gain = (0.7 * height + 0.3 * p.cold + 0.25 * snowing) * math.min(1.5, wind), fade = 0.25}
	wanted.b_trees = {sound = "afx_out_wind_trees",
		gain = hush * 0.6 * p.forest * (0.5 + 0.5 * wind), fade = 0.3}
	-- Birdsong peaks at dawn
	local dawn = math.max(0, 1 - math.abs(ctx.tod - 0.26) / 0.06)
	wanted.b_birds = {sound = "afx_out_birds_far",
		gain = math.max(day, dawn) * calm_birds * (0.25 * p.meadow + 0.35 * p.forest) *
			(1 + 0.8 * dawn) * (1 - height), fade = 0.15}
	-- (the crickets right around you are their own emitters; nature.lua)
	wanted.b_crickets = {sound = "afx_out_crickets",
		gain = night * calm_birds * hush * 0.45 * math.max(p.meadow, p.forest, p.dry) *
			(1 - p.cold) * ((ctx.crickets_near or 0) > 0 and 0.65 or 1), fade = 0.1}
	wanted.b_frogs = {sound = "afx_out_frogs",
		gain = night * 0.4 * math.max(p.swamp, p.coast * 0.5) * (ctx.frogs_quiet and 0 or 1),
		fade = ctx.frogs_quiet and 0.6 or 0.1}
	wanted.b_gulls = {sound = "afx_out_gulls", gain = day * calm_birds * 0.35 * p.coast, fade = 0.15}
	-- the jungle quiets at night (the crickets take over) and under rain
	wanted.b_jungle = {sound = "afx_out_jungle",
		gain = 0.6 * p.jungle * (0.4 + 0.6 * day) * (raining > 0 and 0.3 or 1), fade = 0.2}
	wanted.b_waves = {sound = "afx_out_waves", gain = 0.7 * p.coast, fade = 0.2}
end

--[[ Proximity: things that sound where they are ]]

-- Flowing water nodes, by name (a scan for them, not all water and a
-- get_node on each)
local FLOWING = {}
core.register_on_mods_loaded(function()
	for name, def in pairs(core.registered_nodes) do
		if def.liquidtype == "flowing" and (def.groups or {}).water then
			FLOWING[#FLOWING + 1] = name
		end
	end
end)
afx_ambience.FLOWING = FLOWING

local function proximity_emitters(ppos, wanted, underground)
	local r = CFG.radius + 2
	local lo, hi = vector.subtract(ppos, r), vector.add(ppos, r)
	local day = math.abs(core.get_timeofday() - 0.5) < 0.27

	-- Running water: flowing nodes; a waterfall where flow falls onto flow
	local flowing, falls = {}, {}
	for _, p in ipairs(#FLOWING > 0 and core.find_nodes_in_area(lo, hi, FLOWING) or {}) do
		local below = core.get_node(vector.offset(p, 0, -1, 0)).name
		if below:find("flowing") or below == "air" then
			falls[#falls + 1] = p
		else
			flowing[#flowing + 1] = p
		end
	end
	for _, c in ipairs(nearest(clusters(falls), ppos, 2)) do
		if c.n >= 3 then
			wanted["wf" .. c.key] = {sound = "afx_waterfall", pos = c.pos, gain = math.min(0.9, 0.2 + 0.05 * c.n)}
		end
	end
	for _, c in ipairs(nearest(clusters(flowing), ppos, 3)) do
		-- (underground, water.lua's rivers through the rock have it)
		if c.n >= 2 and not game_has("water_lava") and not underground then
			wanted["rv" .. c.key] = {sound = "afx_river", pos = c.pos, gain = math.min(0.6, 0.1 + 0.04 * c.n)}
		end
	end

	-- Insects over flowers, by day, when it's dry
	if day and afx_ambience.rain_at(ppos) == 0 then
		local flowers = core.find_nodes_in_area(lo, hi, {"group:flower"})
		for _, c in ipairs(nearest(clusters(flowers), ppos, 2)) do
			if c.n >= 3 then
				wanted["fl" .. c.key] = {sound = "afx_insects", pos = c.pos, gain = math.min(0.35, 0.05 * c.n)}
			end
		end
	end

	-- Bees, fires, lit furnaces and campfires
	local hot = core.find_nodes_in_area(lo, hi, hot_nodes())
	local bees = #BEE_NODES > 0 and core.find_nodes_in_area(lo, hi, BEE_NODES) or {}
	local furnaces = #FURNACE_NODES > 0 and core.find_nodes_in_area(lo, hi, FURNACE_NODES) or {}
	for _, c in ipairs(nearest(clusters(furnaces), ppos, 2)) do
		wanted["fu" .. c.key] = {sound = "afx_furnace", pos = c.pos, gain = 0.45, range = 12}
		wanted["fc" .. c.key] = {sound = "afx_fire", pos = c.pos, gain = 0.15, range = 10}
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
	"afx_bird_bluebird", "afx_bird_goldfinch", "afx_bird_goldfinch", "afx_bird_crow"}
local SONGBIRDS = {"afx_bird_robin", "afx_bird_bluebird", "afx_bird_goldfinch",
	"afx_bird_cardinal", "afx_bird_lark"}
local NIGHT_BIRDS = {"afx_bird_owl"}

local function maybe_bird(pname, st, ppos, now, raining)
	if not CFG.birds or now < st.next_bird then return end
	local tod = core.get_timeofday()
	local night = tod < 0.2 or tod > 0.8
	-- Exponential gaps: mostly quiet, sometimes two calls close together
	local tod_ = core.get_timeofday()
	local dawn = math.abs(tod_ - 0.26) < 0.05
	local mean = CFG.bird_interval * (night and 2.5 or 1) * (dawn and 0.25 or 1)
	st.next_bird = now - math.log(math.max(1e-3, rand(0, 1))) * mean + 3
	if raining > 0 then return end
	local leaves = core.find_nodes_in_area(vector.offset(ppos, -20, -2, -20),
			vector.offset(ppos, 20, 14, 20), {"group:leaves"})
	if #leaves < 6 then return end -- no trees, no birds
	-- Birds keep their distance: none right above the player
	local spot
	for _ = 1, 6 do
		spot = pick(leaves)
		if vector.distance(spot, ppos) >= 7 then break end
		spot = nil
	end
	if not spot then return end
	local sound = night and pick(NIGHT_BIRDS) or (dawn and pick(SONGBIRDS) or pick(DAY_BIRDS))
	afx_ambience.play(sound, {pos = spot, to_player = pname, gain = rand(0.35, 0.8),
		pitch = rand(0.92, 1.08), max_hear_distance = 40}, true)
	log(pname .. ": " .. sound .. " at " .. core.pos_to_string(spot))
end

local function maybe_drip(pname, st, ppos, now)
	if not CFG.cave or now < st.next_drip then return end
	st.next_drip = now + rand(1.2, 6)
	local ear = vector.round(vector.offset(ppos, 0, 1.6, 0))
	-- Only where daylight never reaches (a cave, not a cellar with a door)
	local day_light = (core.get_natural_light or core.get_node_light)(ear, 0.5)
	if not day_light or day_light > 3 then return end
	-- Water seeps through the rock where there's water above it: drips fall
	-- from the ceiling right under a lake, a river, an aquifer. A dry cave
	-- only drips now and then.
	local waters = core.find_nodes_in_area(vector.offset(ear, -10, 2, -10),
			vector.offset(ear, 10, 20, 10), {"group:water"})
	local spots = {}
	for i = 1, math.min(#waters, 40) do
		local w = pick(waters)
		-- straight down from the water to the first air: that's the ceiling
		for dy = 1, 6 do
			local p = vector.offset(w, 0, -dy, 0)
			local n = core.get_node_or_nil(p)
			if not n then break end
			if n.name == "air" then
				if dy > 1 then spots[#spots + 1] = vector.offset(p, 0, 0.4, 0) end
				break
			end
		end
		if #spots >= 4 then break end
	end
	local spot
	if #spots > 0 then
		spot = pick(spots)
		st.next_drip = now + rand(0.8, 3.5) -- seeping: drips more often
	else
		if rng:next(1, 100) > 20 then return end
		local ceil = core.find_nodes_in_area(vector.offset(ear, -8, 2, -8),
				vector.offset(ear, 8, 16, 8), {"group:stone", "group:pickaxey", "group:cracky"})
		for _ = 1, 24 do
			if #ceil == 0 then break end
			local p = pick(ceil)
			local below = core.get_node_or_nil(vector.offset(p, 0, -1, 0))
			local def = core.registered_nodes[core.get_node(p).name]
			if below and below.name == "air" and def and (def.light_source or 0) == 0 then
				spot = vector.offset(p, 0, -0.6, 0)
				break
			end
		end
	end
	if not spot then return end
	afx_ambience.play("afx_drip", {pos = spot, to_player = pname,
		gain = rand(0.2, 0.6), pitch = rand(0.85, 1.15), max_hear_distance = 24}, true)
end

--[[ Layers from the other files ]]

local layers, ticks = {}, {}
-- fn(ctx, wanted): add loops to `wanted`; called once a second per player
function afx_ambience.register_layer(fn) layers[#layers + 1] = fn end
-- fn(player, dtime): a fast tick (~5 Hz) for reactions
function afx_ambience.register_tick(fn) ticks[#ticks + 1] = fn end

-- Where the engine's path search found the way out (or nil): exit is the
-- first cell under the sky, inner the doorway, size its open cells across
local function opening(ctx)
	local h = ctx.hearing
	if not h or (h.has_opening or 0) < 0.5 then return nil end
	return {exit = {x = h.opening_x, y = h.opening_y, z = h.opening_z},
		inner = {x = h.inner_x, y = h.inner_y, z = h.inner_z}, size = h.opening_size or 25}
end

afx_ambience.lib = {
	opening = opening,
	CFG = CFG, log = log, rand = rand, pick = pick, clusters = clusters, nearest = nearest,
	sky_open = sky_open, count = count, has = has, rained_on = rained_on,
	player_state = player_state, rain_sound_for = rain_sound_for, game_has = game_has,
}

core.register_on_mods_loaded(function()
	afx_ambience.add_report(function()
		local function list(t) return #t .. " " .. table.concat(t, " ", 1, math.min(#t, 6)) end
		return {"furnaces (lit): " .. list(FURNACE_NODES), "fires/campfires: " .. list(HOT_NODES),
			"bee nests: " .. list(BEE_NODES),
			"game plays its own water/lava: " .. tostring(game_has("water_lava")),
			"game plays its own fire: " .. tostring(game_has("fire"))}
	end)
end)

-- (the server knows the game's dimensions)
local function dimension()
	return afx.world.dimension or "overworld"
end

--[[ Main loop ]]

local timer, tick_timer = 0, 0
local SCAN, TICK = 1.0, 0.2

--[[ Where the player is. The afx engine reports what the player actually
hears (player:get_acoustics(): the voxel probe's enclosure, the path to
the sky, under water...), so this mod and the engine agree. Without it (a
vanilla client) light levels stand in. Fields:
  ctx.under_sky    open sky right above
  ctx.exposure     0..1 how much of the outdoors reaches the ear through air
  ctx.through      0..1 how much reaches it through walls
  ctx.underground  a cave: dark, enclosed, cut off, not a home
  ctx.indoors      a built room (or a lit cellar): enclosed, not a cave
  ctx.hearing      the raw engine report, or nil ]]

local function place_target(ctx)
	local h = ctx.hearing or {}
	if ctx.underground then return 1.0 end
	local stormy = ctx.rain > 0.5 or (ctx.st.wind or 0) > 1.25
	local t
	if ctx.indoors then
		t = ((h.homey or 0) >= 2 or (h.cozy or 0) > 0.4) and 0.6 or 0.75
		if stormy then t = t + 0.15 end -- the storm outside is the drama
	else
		t = stormy and 1.0 or (ctx.day < 0.5 and 0.9 or 0.8)
	end
	return t
end

local function where(ctx)
	local h = ctx.player.get_acoustics and ctx.player:get_acoustics()
	ctx.hearing = h
	if h then
		ctx.under_sky = (h.sky or 0) > 0.5
		ctx.exposure = h.outdoor or 0
		ctx.through = h.wall or 0
		-- a cave is rock around you; a dark plank room is not
		ctx.underground = not ctx.under_sky and ctx.daylight <= 3 and ctx.exposure < 0.15 and
				(h.homey or 0) < 2 and (h.natural or 1) > 0.55
		-- a jungle floor under a dense canopy has no sky right above, but the
		-- open path is wide: that's outdoors
		ctx.indoors = not ctx.under_sky and not ctx.underground and (h.enclosure or 0) > 0.5 and
				ctx.exposure < 0.4
		ctx.underwater = (h.underwater or 0) > 0.5
	else
		ctx.exposure = ctx.under_sky and 1 or math.max(0, (ctx.daylight - 3) / 12)
		ctx.through = 0
		ctx.underground = ctx.daylight <= 3 and not ctx.under_sky
		ctx.indoors = not ctx.under_sky and not ctx.underground
	end
end
afx_ambience.where = where

local function update_place(ctx, dtime)
	local t = place_target(ctx)
	local cur = PLACE[ctx.pname] or t
	PLACE[ctx.pname] = cur + (t - cur) * math.min(1, dtime * 0.3)
	ctx.place_scale = PLACE[ctx.pname]
end

core.register_globalstep(function(dtime)
	tick_timer = tick_timer + dtime
	if tick_timer >= TICK then
		for _, player in ipairs(core.get_connected_players()) do
			for _, fn in ipairs(ticks) do fn(player, tick_timer) end
		end
		tick_timer = 0
	end

	timer = timer + dtime
	if timer < SCAN then return end
	timer = 0
	local now = core.get_us_time() / 1e6
	local tod = core.get_timeofday()
	for _, player in ipairs(core.get_connected_players()) do
		local pname = player:get_player_name()
		local st = player_state(pname)
		local ppos = player:get_pos()
		local ear = vector.round(vector.offset(ppos, 0, 1.6, 0))
		local ctx = {
			player = player, pname = pname, st = st, ppos = ppos, ear = ear, now = now,
			tod = tod, day = math.max(0, math.min(1, (0.32 - math.abs(tod - 0.5)) / 0.05)),
			rain = afx_ambience.rain_at(ppos), snow = afx_ambience.snow_at(ppos),
			dimension = dimension(ppos), under_sky = sky_open(ear),
			daylight = (core.get_natural_light or core.get_node_light)(ear, 0.5) or 0,
		}
		where(ctx)
		update_place(ctx, SCAN)
		local wanted = {}
		for _, fn in ipairs(layers) do fn(ctx, wanted) end
		if CFG.rain_surfaces and ctx.rain > 0 then
			rain_emitters(ctx, ctx.rain * (ctx.rain_ramp or 1), wanted)
		end
		if CFG.lava and not game_has("water_lava") then
			lava_emitters(ppos, wanted)
		end
		if CFG.beds then
			bed_targets(ctx, wanted)
		end
		if CFG.proximity then
			proximity_emitters(ppos, wanted, ctx.underground)
		end
		underwater_bed(player, wanted)
		reconcile(pname, st, wanted)
		-- birds only where any of them would be heard
		if not ctx.underground and ctx.exposure + ctx.through > 0.03 then
			maybe_bird(pname, st, ppos, now, ctx.rain + ctx.snow)
		end
		if ctx.underground then maybe_drip(pname, st, ppos, now) end
	end
end)

core.register_on_leaveplayer(function(player)
	players[player:get_player_name()] = nil
	PLACE[player:get_player_name()] = nil
end)

--[[ From the server: its content's sounds, and test hooks ]]

-- a note stone, a music box, a chime: played here, with their levels
afx.register_on_data("play", function(data)
	local t = core.parse_json(data)
	if type(t) ~= "table" or type(t.name) ~= "string" then return end
	afx_ambience.play(t.name, {pos = t.pos, gain = t.gain, pitch = t.pitch,
		max_hear_distance = t.max_hear_distance}, true)
end)

-- "field value": set a field of the player's state (test scenarios)
afx.register_on_data("poke", function(data)
	local k, v = data:match("^(%S+) (.*)$")
	if not k then return end
	local p = core.get_connected_players()[1]
	if p then player_state(p:get_player_name())[k] = tonumber(v) or v end
end)

-- a command or a formspec's fields, as if typed or clicked (test scenarios)
afx.register_on_data("chat", function(line)
	core.log("action", "afx: client command " .. line .. " -> " .. tostring(core.afx_on_chat(line)))
end)
afx.register_on_data("fields", function(data)
	local t = core.parse_json(data)
	if type(t) ~= "table" then return end
	core.afx_on_formspec(t.formname, t.fields or {})
	core.log("action", "afx: mixer -> reverb " .. tostring(core.settings:get("afx_reverb")) ..
		" birds " .. afx_ambience.volume("birds") ..
		" follower " .. tostring(core.settings:get_bool("afx_ambience_follower")))
end)
