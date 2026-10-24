-- A real-game listening tour for the afx fork, on VoxeLibre's own mapgen,
-- weather, sounds and node definitions (Mineclonia shares the lineage).
--
-- Finds a forest, plains, a snowy biome and a cave in the generated world,
-- then visits them on a fixed timeline: forest at noon, the same forest in
-- rain, plains at dusk into night, a dark cave, snow falling in the cold.

local name
local function P() return core.get_player_by_name(name) end
local function say(msg) core.log("action", "afxplay: " .. msg) end

local function v(x, y, z) return {x = x, y = y, z = z} end

--[[ Finding places ]]

local function biome_at(x, z)
	local bd = core.get_biome_data(v(x, 20, z))
	return bd and core.get_biome_name(bd.biome) or ""
end

local function find_biome(pred, start)
	for r = start or 0, 3000, 96 do
		for a = 0, 15 do
			local x = math.floor(r * math.cos(a * math.pi / 8))
			local z = math.floor(r * math.sin(a * math.pi / 8))
			local b = biome_at(x, z)
			if pred(b) then return v(x, 0, z), b end
		end
	end
end

-- Stand on the ground: scan down for the first walkable node that isn't
-- leaves or a trunk, with two air nodes above it.
local function ground(p)
	for y = 120, -10, -1 do
		local n = core.get_node(v(p.x, y, p.z)).name
		local def = core.registered_nodes[n]
		if def and def.walkable and core.get_item_group(n, "leaves") == 0 and
				core.get_item_group(n, "tree") == 0 then
			return v(p.x, y + 1, p.z)
		end
	end
end

-- A dark cave: an air node underground with room around it.
local function find_cave(center)
	local lo, hi = vector.offset(center, -48, -60, -48), vector.offset(center, 48, -15, 48)
	local airs = core.find_nodes_in_area(lo, hi, {"air"})
	local best, best_n
	for i = 1, math.min(#airs, 4000) do
		local p = airs[math.random(#airs)]
		local below = core.get_node(vector.offset(p, 0, -1, 0)).name
		local def = core.registered_nodes[below]
		if def and def.walkable and (core.get_node_light(p, 0.5) or 15) <= 2 then
			local n = #core.find_nodes_in_area(vector.offset(p, -4, 0, -4),
					vector.offset(p, 4, 6, 4), {"air"})
			if not best_n or n > best_n then best, best_n = p, n end
			if n > 300 then break end
		end
	end
	return best, best_n
end

-- Open air inside another dimension's band [y0, y1]: an air node with
-- solid ground under it and room around, near (0, 0).
local function find_in_band(y0, y1)
	local lo, hi = v(-40, y0, -40), v(40, y1, 40)
	local airs = core.find_nodes_in_area(lo, hi, {"air"})
	for _ = 1, 3000 do
		if #airs == 0 then break end
		local p = airs[math.random(#airs)]
		local below = core.registered_nodes[core.get_node(vector.offset(p, 0, -1, 0)).name]
		if below and below.walkable and below.name ~= "mcl_core:lava_source" and
				core.get_node(vector.offset(p, 0, 1, 0)).name == "air" then
			local n = #core.find_nodes_in_area(vector.offset(p, -3, 0, -3),
					vector.offset(p, 3, 4, 3), {"air"})
			if n > 120 then return p end
		end
	end
end

local function emerge(center, r, cb)
	core.emerge_area(vector.offset(center, -r, -70, -r), vector.offset(center, r, 90, r),
		function(_, _, remaining) if remaining == 0 then cb() end end)
end

--[[ Timeline ]]

local function set_time(from, to, seconds)
	local t = 0
	local function tick()
		t = t + 0.25
		local k = math.min(1, t / seconds)
		core.set_timeofday(from + (to - from) * k)
		if k < 1 then core.after(0.25, tick) end
	end
	tick()
end

local function go(pos, look)
	local pl = P()
	pl:set_pos(pos)
	pl:set_look_horizontal(look or 0)
	pl:set_look_vertical(0)
end

local function weather(w)
	mcl_weather.change_weather(w, core.get_gametime() + 3600, "afxplay")
end

local SPOTS = {}

local function run(steps, i)
	i = i or 1
	local s = steps[i]
	if not s then
		say("done")
		core.request_shutdown("afxplay done", false, 1)
		return
	end
	say(s.log)
	s.fn()
	core.after(s.wait, run, steps, i + 1)
end

local function tour()
	say("start")
	run({
		{log = "forest, noon", wait = 16, fn = function()
			weather("none"); core.set_timeofday(0.5); go(SPOTS.forest) end},
		{log = "forest, rain", wait = 16, fn = function() weather("rain") end},
		{log = "plains, dusk into night", wait = 26, fn = function()
			weather("none"); go(SPOTS.plains); set_time(0.72, 0.85, 16) end},
		{log = "cave", wait = 28, fn = function() core.set_timeofday(0.5); go(SPOTS.cave) end},
		{log = "snow", wait = 16, fn = function() go(SPOTS.snow); weather("snow") end},
		{log = "thunderstorm", wait = 6, fn = function()
			go(SPOTS.forest); core.set_timeofday(0.5); weather("thunder") end},
		{log = "far strike", wait = 9, fn = function()
			lightning.strike(vector.offset(SPOTS.forest, 420, 0, 300)) end},
		{log = "near strike", wait = 12, fn = function()
			lightning.strike(vector.offset(SPOTS.forest, 40, 0, -50)) end},
		{log = "nether", wait = 20, fn = function()
			weather("none"); if SPOTS.nether then go(SPOTS.nether) end end},
		{log = "the end", wait = 22, fn = function() if SPOTS.the_end then go(SPOTS.the_end) end end},
		{log = "end", wait = 1, fn = function() weather("none") end},
	})
end

core.register_on_joinplayer(function(player)
	name = player:get_player_name()
	core.settings:set("time_speed", "0")
	player:set_physics_override({gravity = 0, speed = 0, jump = 0})
	weather("none")

	local f, fb = find_biome(function(b) return b == "Forest" or b == "FlowerForest" end)
	local p, pb = find_biome(function(b) return b == "Plains" or b == "SunflowerPlains" end)
	local s, sb = find_biome(function(b) return b == "IcePlains" or b == "ColdTaiga" end)
	say(("found %s %s, %s %s, %s %s"):format(fb, core.pos_to_string(f or v(0,0,0)),
		pb, core.pos_to_string(p or v(0,0,0)), sb, core.pos_to_string(s or v(0,0,0))))
	local todo
	local function done()
		todo = todo - 1
		if todo > 0 then return end
		SPOTS.forest = ground(f)
		SPOTS.plains = ground(p)
		SPOTS.snow = ground(s)
		local cave, n = find_cave(p)
		SPOTS.cave = cave and vector.offset(cave, 0, 0, 0) or vector.offset(SPOTS.plains, 0, -30, 0)
		say(("spots forest %s plains %s snow %s cave %s (%s air)"):format(
			core.pos_to_string(SPOTS.forest), core.pos_to_string(SPOTS.plains),
			core.pos_to_string(SPOTS.snow), core.pos_to_string(SPOTS.cave), tostring(n)))
		core.after(1, tour)
	end
	todo = 5
	emerge(f, 24, done)
	emerge(p, 48, done)
	emerge(s, 24, done)
	local nmin, nmax = mcl_vars.mg_nether_min, mcl_vars.mg_nether_max
	core.emerge_area(v(-48, nmin + 20, -48), v(48, nmin + 80, 48), function(_, _, rem)
		if rem > 0 then return end
		SPOTS.nether = find_in_band(nmin + 25, nmin + 75)
		say("nether spot " .. core.pos_to_string(SPOTS.nether or v(0, 0, 0)))
		done()
	end)
	local emin = mcl_vars.mg_end_min
	core.emerge_area(v(-48, emin + 40, -48), v(48, emin + 100, 48), function(_, _, rem)
		if rem > 0 then return end
		SPOTS.the_end = find_in_band(emin + 45, emin + 95)
		say("end spot " .. core.pos_to_string(SPOTS.the_end or v(0, 0, 0)))
		done()
	end)
end)
