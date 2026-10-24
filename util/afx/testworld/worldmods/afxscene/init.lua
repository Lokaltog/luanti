-- Scripted listening scenarios for the afx fork (see util/afx/tour.sh).
--
-- Each scenario builds a small place, plays the beds a game would play
-- (rain, wind) and walks the player through it on a fixed timeline, with
-- footsteps. The afx_ambience mod supplies the world-driven sounds (rain on
-- roofs and windows, birds, drips, lava) and the engine the acoustics.
--
--   afxscene_scenario = hut_rain | tunnel_wind | forest | cavern | cliff

local SCENARIO = core.settings:get("afxscene_scenario") or "hut_rain"

local function v(x, y, z) return {x = x, y = y, z = z} end

--[[ Nodes with the groups real games use ]]

core.register_node("afxscene:planks", {
	description = "Planks", tiles = {"[fill:16x16:#a07040"},
	groups = {wood = 1, axey = 1, choppy = 2},
	sounds = {footstep = {name = "default_wood_footstep", gain = 0.5}},
})
core.register_node("afxscene:glass", {
	description = "Glass", drawtype = "glasslike", tiles = {"[fill:16x16:#c0e0ff40"},
	use_texture_alpha = "blend", paramtype = "light", sunlight_propagates = true,
	groups = {glass = 1, cracky = 3},
})
core.register_node("afxscene:leaves", {
	description = "Leaves", drawtype = "allfaces_optional", tiles = {"[fill:16x16:#3a7a30"},
	paramtype = "light", sunlight_propagates = true, groups = {leaves = 1, snappy = 3},
})
core.register_node("afxscene:trunk", {
	description = "Trunk", tiles = {"[fill:16x16:#604020"}, groups = {tree = 1, choppy = 2},
})
-- One node of a door: a thin slab. facedir 0 closes the doorway (blocks Z),
-- facedir 1 is swung open (blocks X only).
core.register_node("afxscene:door", {
	description = "Door", drawtype = "nodebox", tiles = {"[fill:16x16:#805030"},
	paramtype = "light", paramtype2 = "facedir",
	node_box = {type = "fixed", fixed = {-0.5, -0.5, -0.5, 0.5, 0.5, -0.375}},
	groups = {wood = 1, axey = 1},
})

--[[ Building ]]

local MIN, MAX = v(-48, -24, -48), v(48, 40, 48)

local function builder()
	local vm = core.get_voxel_manip()
	local emin, emax = vm:read_from_map(MIN, MAX)
	local area = VoxelArea(emin, emax)
	local data = vm:get_data()
	local b = {}
	function b.fill(a, z, name)
		local c = core.get_content_id(name)
		for zz = math.min(a.z, z.z), math.max(a.z, z.z) do
			for yy = math.min(a.y, z.y), math.max(a.y, z.y) do
				for xx = math.min(a.x, z.x), math.max(a.x, z.x) do
					if area:contains(xx, yy, zz) then data[area:index(xx, yy, zz)] = c end
				end
			end
		end
	end
	function b.done()
		vm:set_data(data)
		vm:write_to_map(true)
		-- a fresh light pass over everything we touched
		core.fix_light(MIN, MAX)
	end
	b.fill(MIN, MAX, "air")
	return b
end

local function tree(b, x, z, h)
	b.fill(v(x, 0, z), v(x, h, z), "afxscene:trunk")
	b.fill(v(x - 2, h - 1, z - 2), v(x + 2, h + 1, z + 2), "afxscene:leaves")
	b.fill(v(x - 1, h + 2, z - 1), v(x + 1, h + 2, z + 1), "afxscene:leaves")
	b.fill(v(x, h - 1, z), v(x, h, z), "afxscene:trunk")
end

local BUILD = {}

function BUILD.hut_rain(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	b.fill(v(-4, -1, -4), v(4, -1, 4), "afxscene:planks")    -- floor
	b.fill(v(-4, 0, -4), v(4, 5, 4), "afxscene:planks")      -- shell
	b.fill(v(-3, 0, -3), v(3, 4, 3), "air")                  -- room
	b.fill(v(4, 2, -1), v(4, 3, 1), "afxscene:glass")        -- east window
	b.fill(v(-4, 2, -1), v(-4, 3, 1), "afxscene:glass")      -- west window
	b.fill(v(0, 0, -4), v(0, 1, -4), "air")                  -- doorway
	tree(b, 12, 6, 5)
end

function BUILD.tunnel_wind(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:stone")
	b.fill(v(-40, 0, -25), v(11, 22, 25), "basenodes:stone")
	b.fill(v(-24, 0, -1), v(11, 2, 1), "air")
end

function BUILD.forest(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	local spots = {{9, 4}, {-8, 7}, {14, -6}, {-12, -10}, {4, 16}, {-18, 3}, {20, 12}, {-4, -17}}
	for i, s in ipairs(spots) do tree(b, s[1], s[2], 4 + i % 3) end
end

function BUILD.cavern(b)
	b.fill(v(MIN.x, -24, MIN.z), v(MAX.x, 30, MAX.z), "basenodes:stone")
	b.fill(v(-14, -8, -14), v(14, 4, 14), "air")
	b.fill(v(6, -9, 6), v(12, -9, 12), "basenodes:lava_source")
	b.fill(v(6, -8, 6), v(12, -8, 12), "air")
end

function BUILD.cliff(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, 0, 20), v(MAX.x, 30, 30), "basenodes:stone")
end

--[[ Timeline helpers ]]

local player_name
local function P() return core.get_player_by_name(player_name) end

local steps = {}

local function footstep(kind)
	core.sound_play(kind, {to_player = player_name, gain = 0.6, pitch = 0.95 + math.random() * 0.1}, true)
end

-- Walk in straight segments at `speed` nodes/s with footsteps every 0.5 s.
local function walk(to, speed, kind, done)
	local p = P()
	local from = p:get_pos()
	local dist = vector.distance(from, to)
	local dur = dist / (speed or 3)
	local t, step_t = 0, 0
	local function tick()
		local pl = P()
		if not pl then return end
		t = t + 0.05
		step_t = step_t + 0.05
		local k = math.min(1, t / dur)
		pl:set_pos(vector.add(from, vector.multiply(vector.subtract(to, from), k)))
		if step_t >= 0.5 then
			step_t = 0
			footstep(type(kind) == "function" and kind(pl:get_pos()) or kind)
		end
		if k < 1 then core.after(0.05, tick) elseif done then done() end
	end
	tick()
end

local function run(timeline, i)
	i = i or 1
	local item = timeline[i]
	if not item then
		core.log("action", "afxscene: done")
		core.request_shutdown("afxscene done", false, 1)
		return
	end
	if item.log then core.log("action", "afxscene: " .. item.log) end
	if item.walk then
		walk(item.walk, item.speed, item.steps, function() run(timeline, i + 1) end)
	else
		if item.fn then item.fn() end
		core.after(item.wait or 0, run, timeline, i + 1)
	end
end

local function bed(name, gain)
	core.sound_play(name, {to_player = player_name, loop = true, gain = gain})
end

local function hut_steps(pos)
	if math.abs(pos.x) <= 4 and math.abs(pos.z) <= 4 then return "default_wood_footstep" end
	return "default_grass_footstep"
end

local function tap()
	core.sound_play("default_dig_cracky", {to_player = player_name, gain = 0.8}, true)
end

local function door(open)
	for y = 0, 1 do
		core.set_node(v(0, y, -4), {name = "afxscene:door", param2 = open and 1 or 0})
	end
end

local TIMELINE = {
	hut_rain = {
		{fn = function()
			afx_ambience.set_rain(0.8)
			bed("afx_out_rain", 0.8)
			door(true)
		end, wait = 4, log = "outside in the rain"},
		{walk = v(0, 0, -5.5), speed = 2.5, steps = hut_steps, log = "to the door"},
		{walk = v(0, 0, 0.5), speed = 2.5, steps = hut_steps, log = "inside, door open"},
		{wait = 6},
		{fn = function() door(false) end, wait = 9, log = "door shut"},
		{fn = function() door(true) end, wait = 3, log = "door open"},
		{walk = v(0, 0, -14), speed = 3, steps = hut_steps, log = "back out"},
		{wait = 4},
	},
	tunnel_wind = {
		{fn = function() bed("afx_out_wind", 0.8) end, wait = 3, log = "outside"},
		{walk = v(12, 0, 0), speed = 3, steps = "default_hard_footstep", log = "to the mouth"},
		{walk = v(-18, 0, 0), speed = 3, steps = "default_hard_footstep", log = "deep in"},
		{wait = 2},
		{fn = tap, wait = 0.8}, {fn = tap, wait = 0.8}, {fn = tap, wait = 3},
		{walk = v(22, 0, 0), speed = 4, steps = "default_hard_footstep", log = "back out"},
		{wait = 3},
	},
	forest = {
		{fn = function() bed("afx_out_wind_trees", 0.6) end, wait = 50, log = "standing among trees"},
	},
	cavern = {
		{fn = function()
			bed("afx_out_wind", 0.8) -- the game's outdoor bed: should be gone in here
			bed("afx_in_cave", 0.5)
		end, wait = 8, log = "in the cavern"},
		{walk = v(4, -8, 2), speed = 1.8, steps = "default_hard_footstep", log = "toward the lava"},
		{wait = 3},
		{fn = tap, wait = 0.9}, {fn = tap, wait = 0.9}, {fn = tap, wait = 8},
		{walk = v(-10, -8, -10), speed = 2, steps = "default_hard_footstep", log = "away"},
		{wait = 6},
	},
	cliff = {
		{fn = function() bed("afx_out_wind", 0.5) end, wait = 3, log = "facing the cliff"},
		{fn = tap, wait = 1.5}, {fn = tap, wait = 1.5}, {fn = tap, wait = 2},
		{walk = v(0, 0, 14), speed = 2.5, steps = "default_hard_footstep", log = "closer"},
		{fn = tap, wait = 1.5}, {fn = tap, wait = 3},
	},
}

local START = {
	hut_rain = v(0, 0, -14), tunnel_wind = v(22, 0, 0), forest = v(0, 0, 0),
	cavern = v(-10, -8, -10), cliff = v(0, 0, 0),
}

core.register_on_joinplayer(function(player)
	player_name = player:get_player_name()
	core.set_timeofday(0.5)
	core.settings:set("time_speed", "0")
	player:set_physics_override({gravity = 0, speed = 0, jump = 0})
	player:set_look_horizontal(0)
	player:set_look_vertical(0)
	core.emerge_area(MIN, MAX, function(_, _, remaining)
		if remaining > 0 then return end
		local b = builder()
		BUILD[SCENARIO](b)
		b.done()
		core.log("action", "afxscene: built " .. SCENARIO)
		local pl = P()
		pl:set_pos(START[SCENARIO])
		core.after(1.5, function()
			core.log("action", "afxscene: start")
			run(TIMELINE[SCENARIO])
		end)
	end)
end)
