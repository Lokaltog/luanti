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
-- rock the hand can't dig: punching it plays only the knock layer
core.register_node("afxscene:bedrock", {
	description = "Rock", tiles = {"[fill:16x16:#707070"},
	groups = {stone = 1},
	sounds = {footstep = {name = "default_hard_footstep", gain = 0.5}},
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

-- Signs of home (the engine's mood reads these) and other props
core.register_node("afxscene:bed", {description = "Bed", tiles = {"[fill:16x16:#c03030"},
	groups = {bed = 1, wool = 1}})
core.register_node("afxscene:chest", {description = "Chest", tiles = {"[fill:16x16:#906030"},
	groups = {container = 1, wood = 1}})
core.register_node("afxscene:furnace_active", {description = "Furnace", tiles = {"[fill:16x16:#806050"},
	light_source = 12, groups = {furnace = 1, cracky = 2}})
core.register_node("afxscene:lamp", {description = "Lamp", tiles = {"[fill:16x16:#ffe0a0"},
	light_source = 14, groups = {cracky = 3}})
core.register_node("afxscene:flower", {description = "Flower", drawtype = "plantlike",
	tiles = {"[fill:16x16:#ff60a0"}, paramtype = "light", sunlight_propagates = true,
	walkable = false, groups = {flower = 1, snappy = 3}})
core.register_node("afxscene:sand", {description = "Sand", tiles = {"[fill:16x16:#e0d090"},
	groups = {sand = 1, crumbly = 3}})

core.register_node("afxscene:clock", {description = "Wall clock", drawtype = "signlike",
	tiles = {"[fill:16x16:#f0e8d0"}, paramtype = "light", paramtype2 = "wallmounted",
	walkable = false, groups = {clock = 1, attached_node = 1}})
core.register_node("afxscene:rail", {description = "Rail", drawtype = "raillike",
	tiles = {"[fill:16x16:#806040"}, paramtype = "light", walkable = false, groups = {rail = 1}})
core.register_node("afxscene:fence", {description = "Fence", drawtype = "fencelike",
	tiles = {"[fill:16x16:#a07040"}, paramtype = "light", groups = {fence = 1, wood = 1}})
core.register_node("afxscene:spawner", {description = "Spawner", drawtype = "glasslike",
	tiles = {"[fill:16x16:#202830"}, groups = {cracky = 1}})
core.register_node("afxscene:mossy_cobble", {description = "Mossy cobble",
	tiles = {"[fill:16x16:#506850"}, groups = {cracky = 2, stone = 1}})

core.register_node("afxscene:tallgrass", {description = "Tall grass", drawtype = "plantlike",
	tiles = {"[fill:16x16:#4a8a30"}, paramtype = "light", sunlight_propagates = true,
	walkable = false, buildable_to = true, groups = {grass = 1, flora = 1, snappy = 3}})
core.register_node("afxscene:bell", {description = "Bell", tiles = {"[fill:16x16:#d0a020"},
	groups = {cracky = 2}})
core.register_craftitem("afxscene:pocket_clock", {description = "Clock",
	inventory_image = "[fill:16x16:#d0c060"})

--[[ Building ]]

local MIN, MAX = v(-48, -40, -48), v(64, 96, 48)

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

function BUILD.descent(b)
	b.fill(v(MIN.x, -40, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- home
	b.fill(v(-4, -1, -4), v(4, -1, 4), "afxscene:planks")
	b.fill(v(-4, 0, -4), v(4, 5, 4), "afxscene:planks")
	b.fill(v(-3, 0, -3), v(3, 4, 3), "air")
	b.fill(v(0, 0, -4), v(0, 1, -4), "air")
	b.fill(v(-2, 0, 2), v(-1, 0, 2), "afxscene:bed")
	b.fill(v(2, 0, 3), v(2, 0, 3), "afxscene:chest")
	b.fill(v(3, 0, -2), v(3, 0, -2), "afxscene:furnace_active")
	b.fill(v(-3, 3, 0), v(-3, 3, 0), "afxscene:lamp")
	b.fill(v(3, 3, 0), v(3, 3, 0), "afxscene:lamp")
	b.fill(v(0, 4, 0), v(0, 4, 0), "afxscene:lamp")
	-- a ramp down into the dark
	for i = 0, 24 do
		b.fill(v(10 + i, -1 - i, -1), v(10 + i, 2 - i, 1), "air")
	end
	b.fill(v(34, -24, -14), v(62, -12, 14), "air")
end

function BUILD.meadow(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	for _, f in ipairs({{3, 2}, {4, 3}, {2, 4}, {5, 1}, {-3, 5}, {-4, 4}, {-2, 6}, {6, -3}, {7, -2}, {5, -4}}) do
		b.fill(v(f[1], 0, f[2]), v(f[1], 0, f[2]), "afxscene:flower")
	end
	tree(b, 16, 10, 5)
	tree(b, -15, 14, 6)
	tree(b, 12, -16, 5)
end

function BUILD.beach(b)
	b.fill(v(MIN.x, -12, MIN.z), v(MAX.x, -7, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, 6), "afxscene:sand")
	b.fill(v(MIN.x, -6, 7), v(MAX.x, -2, MAX.z), "basenodes:water_source")
	-- a cliff with a waterfall
	b.fill(v(-24, 0, -10), v(-14, 8, 0), "basenodes:stone")
	b.fill(v(-16, 8, -6), v(-16, 8, -6), "basenodes:water_source")
	b.fill(v(-15, 0, -6), v(-15, 8, -6), "basenodes:water_flowing")
	b.fill(v(-14, 0, -6), v(-10, 0, -6), "basenodes:water_flowing")
end

function BUILD.sky_island(b)
	for x = -10, 10 do
		for z = -10, 10 do
			local d = math.sqrt(x * x + z * z)
			if d <= 10 then
				local depth = math.floor((10 - d) * 0.8) + 1
				b.fill(v(x, 79 - depth, z), v(x, 78, z), "basenodes:stone")
				b.fill(v(x, 79, z), v(x, 79, z), "basenodes:dirt_with_grass")
			end
		end
	end
	b.fill(v(1, 83, 1), v(5, 85, 5), "afxscene:leaves")
	b.fill(v(3, 80, 3), v(3, 85, 3), "afxscene:trunk")
end

function BUILD.storm(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- an open shelter: four posts and a plank roof
	for _, c in ipairs({{-3, -3}, {3, -3}, {-3, 3}, {3, 3}}) do
		b.fill(v(c[1], 0, c[2]), v(c[1], 3, c[2]), "afxscene:trunk")
	end
	b.fill(v(-4, 4, -4), v(4, 4, 4), "afxscene:planks")
	tree(b, 12, 2, 5)
	tree(b, 15, -3, 6)
	tree(b, 11, -7, 5)
	tree(b, 16, 5, 5)
end

function BUILD.shore(b)
	b.fill(v(MIN.x, -10, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a lake to the north, a sandy bank
	b.fill(v(-24, -5, 6), v(24, -1, 40), "basenodes:water_source")
	b.fill(v(-24, -1, 4), v(24, -1, 5), "afxscene:sand")
	tree(b, -6, -4, 5)
	tree(b, -10, -9, 6)
	tree(b, 4, -10, 5)
	tree(b, 9, -5, 5)
end

function BUILD.home(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	b.fill(v(-5, -1, -5), v(5, -1, 5), "afxscene:planks")
	b.fill(v(-5, 0, -5), v(5, 4, 5), "afxscene:planks")
	b.fill(v(-4, 0, -4), v(4, 3, 4), "air")
	b.fill(v(-6, 5, -6), v(6, 5, 6), "afxscene:planks")          -- roof with eaves
	b.fill(v(5, 1, -1), v(5, 2, 1), "afxscene:glass")             -- east window
	b.fill(v(0, 0, -5), v(0, 1, -5), "air")                       -- doorway
	b.fill(v(-3, 0, 3), v(-2, 0, 3), "afxscene:bed")
	b.fill(v(3, 0, 4), v(3, 0, 4), "afxscene:chest")
	b.fill(v(4, 0, -3), v(4, 0, -3), "afxscene:furnace_active")
	b.fill(v(-4, 2, 0), v(-4, 2, 0), "afxscene:lamp")
	b.fill(v(-4, 2, 3), v(-4, 2, 3), "afxscene:clock")
end

function BUILD.mineshaft(b)
	b.fill(v(MIN.x, -30, MIN.z), v(MAX.x, 10, MAX.z), "basenodes:stone")
	-- a long gallery with rails and timber frames
	b.fill(v(-30, -20, -1), v(30, -18, 1), "air")
	b.fill(v(-30, -20, 0), v(30, -20, 0), "afxscene:rail")
	for x = -28, 28, 4 do
		b.fill(v(x, -20, -1), v(x, -19, -1), "afxscene:fence")
		b.fill(v(x, -20, 1), v(x, -19, 1), "afxscene:fence")
		b.fill(v(x, -18, -1), v(x, -18, 1), "afxscene:planks")
	end
	-- a dungeon room off its east end
	b.fill(v(34, -21, -5), v(44, -16, 5), "afxscene:mossy_cobble")
	b.fill(v(35, -20, -4), v(43, -17, 4), "air")
	b.fill(v(31, -20, 0), v(34, -19, 0), "air")
	b.fill(v(39, -20, 0), v(39, -20, 0), "afxscene:spawner")
end

function BUILD.steps(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
end

function BUILD.corner(b)
	b.fill(v(MIN.x, -12, MIN.z), v(MAX.x, 20, MAX.z), "basenodes:stone")
	b.fill(v(-20, 0, -1), v(4, 2, 1), "air")       -- east-west gallery
	b.fill(v(2, 0, 2), v(4, 2, 18), "air")         -- turning north at its end
end

function BUILD.nextdoor(b)
	b.fill(v(MIN.x, -20, MIN.z), v(MAX.x, 30, MAX.z), "basenodes:stone")
	b.fill(v(-14, -8, 2), v(14, 10, 28), "air")    -- a big cavern to the north
	b.fill(v(-4, -1, -8), v(4, 4, 1), "afxscene:planks")
	b.fill(v(-3, 0, -7), v(3, 3, 0), "air")        -- a hut dug into the rock
	b.fill(v(0, 0, 1), v(0, 1, 1), "air")          -- its door to the cavern
end

function BUILD.voices(b)
	b.fill(v(MIN.x, -30, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a sealed wooden hut off to the west
	b.fill(v(-29, 0, -25), v(-19, 5, -15), "afxscene:planks")
	b.fill(v(-28, 0, -24), v(-20, 4, -16), "air")
	-- a rocky hill to the north with a deep cave in it, its mouth facing us
	b.fill(v(-14, 0, 12), v(14, 14, 44), "basenodes:stone")
	b.fill(v(-9, 0, 18), v(9, 11, 40), "air")
	b.fill(v(-2, 0, 12), v(2, 3, 18), "air")
	-- a pond to the east, with something in it
	b.fill(v(8, -5, -8), v(18, -1, 2), "basenodes:water_source")
end

function BUILD.knock(b)
	b.fill(v(MIN.x, -20, MIN.z), v(MAX.x, 20, MAX.z), "afxscene:bedrock")
	b.fill(v(-8, 0, -2), v(8, 2, 0), "air")          -- a tunnel running east-west
	-- behind the north wall: solid at x < 0; at x > 0 a cave, 2 nodes in
	b.fill(v(2, -3, 3), v(10, 5, 12), "air")
end

function BUILD.music(b)
	b.fill(v(MIN.x, -30, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a stair of note stones out in the meadow, going up the scale
	for i = 1, 8 do
		b.fill(v(-12 + i, 0, 0), v(-12 + i, i - 1, 0), "afx_ambience:note_stone_" .. i)
	end
	b.fill(v(4, 0, 4), v(4, 0, 4), "afx_ambience:music_box")
	-- a great cavern below, with its own stair and a music box
	b.fill(v(-16, -28, -16), v(16, -12, 16), "air")
	for i = 1, 8 do
		b.fill(v(-6 + i, -28, 2), v(-6 + i, -29 + i, 2), "afx_ambience:note_stone_" .. i)
	end
	b.fill(v(6, -28, 6), v(6, -28, 6), "afx_ambience:music_box")
end

function BUILD.walls(b)
	b.fill(v(MIN.x, -20, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a long stone wall to the north of the path
	b.fill(v(-24, 0, 2), v(-4, 4, 2), "basenodes:stone")
	-- then a corridor between two walls, open to the sky
	b.fill(v(-2, 0, -2), v(14, 4, -2), "basenodes:stone")
	b.fill(v(-2, 0, 2), v(14, 4, 2), "basenodes:stone")
end

function BUILD.accents(b)
	b.fill(v(MIN.x, -20, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a sealed hut with a door (south, z=-4) and a window
	b.fill(v(-4, 0, -4), v(4, 5, 4), "afxscene:planks")
	b.fill(v(-3, 0, -3), v(3, 4, 3), "air")
	b.fill(v(4, 2, 0), v(4, 2, 1), "afxscene:glass")
	for y = 0, 1 do b.fill(v(0, y, -4), v(0, y, -4), "afxscene:door") end
	-- a lava pool out in the open, and a plank deck over a drop
	b.fill(v(8, -1, -14), v(11, -1, -11), "basenodes:lava_source")
	b.fill(v(-12, -6, -14), v(-8, -1, -10), "air")
	b.fill(v(-12, -1, -14), v(-8, -1, -10), "afxscene:planks")
end

core.register_entity("afxscene:dart", {
	initial_properties = {physical = false, pointable = false, visual = "sprite",
		textures = {"[fill:4x4:#ff0000"}, visual_size = {x = 0.2, y = 0.2}, static_save = false},
	on_step = function(self, dtime)
		self.t = (self.t or 0) + dtime
		if self.t > 3 then self.object:remove() end
	end,
})

function BUILD.nature(b)
	b.fill(v(MIN.x, -30, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a wood to the west
	for _, t in ipairs({{-44, -30}, {-38, -24}, {-32, -34}, {-26, -26}, {-40, -40}, {-30, -18},
			{-20, -32}, {-44, -16}, {-24, -42}, {-36, -10}}) do tree(b, t[1], t[2], 6) end
	-- a meadow of tall grass in the middle south
	b.fill(v(-14, 0, -46), v(14, 0, -20), "afxscene:tallgrass")
	-- a frozen lake to the south east
	b.fill(v(22, -1, -44), v(44, -1, -24), "basenodes:ice")
	-- a ridge running north-south at x = 0..6, north half
	b.fill(v(0, 0, 0), v(6, 24, 46), "basenodes:stone")
	-- a cave in a hill to the north east, its mouth facing west
	b.fill(v(30, 0, 10), v(60, 16, 46), "basenodes:stone")
	b.fill(v(36, 0, 18), v(56, 10, 40), "air")
	b.fill(v(30, 0, 28), v(36, 2, 30), "air")
end

function BUILD.bridge(b)
	b.fill(v(MIN.x, -20, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:stone")
	b.fill(v(-30, -1, -1), v(-30, -1, 1), "basenodes:stone")
	b.fill(v(-6, -20, -10), v(6, -1, 10), "air")   -- a ravine
	b.fill(v(-6, -1, -1), v(6, -1, 1), "afxscene:planks") -- the bridge
end

function BUILD.draft(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	b.fill(v(-4, 0, -4), v(4, 4, 4), "afxscene:planks")
	b.fill(v(-3, 0, -3), v(3, 3, 3), "air")
	b.fill(v(4, 2, 0), v(4, 2, 0), "air")          -- a one-block gap in the east wall
end

function BUILD.details(b)
	b.fill(v(MIN.x, -40, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	-- a meadow of tall grass (two high) to wade through
	b.fill(v(-20, 0, -4), v(-8, 1, 4), "afxscene:tallgrass")
	-- a porch: posts and a roof, a wind chime hanging under its edge
	b.fill(v(0, 0, 8), v(0, 3, 8), "afxscene:trunk")
	b.fill(v(4, 0, 8), v(4, 3, 8), "afxscene:trunk")
	b.fill(v(-1, 4, 5), v(5, 4, 9), "afxscene:planks")
	b.fill(v(2, 3, 9), v(2, 3, 9), "afx_ambience:wind_chime")
	-- a bell tower in the distance
	b.fill(v(30, 0, 30), v(32, 8, 32), "basenodes:cobble")
	b.fill(v(31, 9, 31), v(31, 9, 31), "afxscene:bell")
	-- a deep pool to fall into, a lake over a cave
	b.fill(v(-30, -12, 20), v(-20, -1, 30), "basenodes:water_source")
	b.fill(v(10, -14, -30), v(30, -2, -10), "basenodes:water_source")   -- lake
	b.fill(v(10, -30, -30), v(30, -20, -10), "air")                    -- cave under it
end

function BUILD.cinema(b)
	b.fill(v(MIN.x, -40, MIN.z), v(MAX.x, -2, MAX.z), "basenodes:stone")
	b.fill(v(MIN.x, -1, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
	tree(b, 8, 6, 5)
	tree(b, -9, 10, 6)
	b.fill(v(-12, -30, -12), v(12, -20, 12), "air") -- a dark cavern far below
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

local function set_time(from, to, seconds, done)
	local t = 0
	local function tick()
		t = t + 0.2
		local k = math.min(1, t / seconds)
		core.set_timeofday(from + (to - from) * k)
		if k < 1 then core.after(0.2, tick) elseif done then done() end
	end
	tick()
end

TIMELINE.descent = {
	{wait = 8, log = "home, lamps lit"},
	{walk = v(0, 0, -6), speed = 2.5, steps = hut_steps, log = "out the door"},
	{walk = v(9, 0, 0), speed = 3, steps = "default_grass_footstep"},
	{walk = v(34, -24, 0), speed = 2.5, steps = "default_hard_footstep", log = "down into the dark"},
	{walk = v(46, -24, 2), speed = 2, steps = "default_hard_footstep", log = "in the cavern"},
	{wait = 35, log = "waiting in the dark"},
	{walk = v(34, -24, 0), speed = 3, steps = "default_hard_footstep", log = "back"},
	{walk = v(9, 0, 0), speed = 3.5, steps = "default_hard_footstep", log = "up the ramp"},
	{walk = v(0, 0, -12), speed = 3, steps = "default_grass_footstep", log = "out under the sky"},
	{wait = 10},
}

TIMELINE.meadow = {
	{wait = 18, log = "noon in a meadow"},
	{fn = function() set_time(0.5, 0.86, 12) end, wait = 12, log = "dusk"},
	{wait = 25, log = "night"},
}

TIMELINE.beach = {
	{wait = 10, log = "on the beach"},
	{walk = v(-11, 0, -4), speed = 3, steps = "default_grass_footstep", log = "to the waterfall"},
	{wait = 7},
	{walk = v(0, 0, 5), speed = 3, steps = "default_grass_footstep", log = "to the sea"},
	{walk = v(0, -4.5, 12), speed = 2, log = "under water"},
	{wait = 8},
	{walk = v(0, 0, 4), speed = 2.5, log = "back out"},
	{wait = 5},
}

TIMELINE.sky_island = {
	{fn = function() bed("afx_out_wind", 0.0) end, wait = 12, log = "on a floating island"},
	{walk = v(9, 80, 0), speed = 2, steps = "default_grass_footstep", log = "to the edge"},
	{wait = 12},
}

local function strike(dist, dir)
	local p = P():get_pos()
	local s = vector.add(p, vector.multiply(dir or v(0.6, 0, 0.8), dist))
	core.log("action", "afxscene: strike at " .. math.floor(dist) .. " nodes")
	afx_ambience.thunder_at(s)
end

local function dig(pos)
	local pl = P()
	local node = core.get_node(pos)
	if node.name ~= "air" then core.node_dig(pos, node, pl) end
end

TIMELINE.storm = {
	{fn = function() afx_ambience.gust_now(1.4) end, wait = 12, log = "windy, a gust"},
	{fn = function() afx_ambience.set_rain(0.8) end, wait = 1,
		log = "rain begins"},
	{fn = function()
		-- the game's rain bed swells in (afx_ambience ramps the surfaces)
		local h = core.sound_play("afx_out_rain", {to_player = player_name, loop = true, gain = 0.05})
		core.sound_fade(h, 0.04, 0.8)
	end, wait = 14},
	{fn = function() strike(700) end, wait = 9, log = "far strike"},
	{fn = function() strike(90, v(-0.8, 0, 0.6)) end, wait = 10, log = "near strike"},
	{walk = v(0, 0, 0), speed = 3, steps = "default_grass_footstep", log = "under the shelter"},
	{wait = 8},
	{fn = function() afx_ambience.set_rain(0) end, wait = 3, log = "rain stops"},
	{walk = v(13, 0, 0), speed = 2.5, steps = "default_grass_footstep", log = "under the trees"},
	{wait = 16},
}

TIMELINE.shore = {
	{fn = function() core.set_timeofday(0.18) end, wait = 12, log = "night by the lake"},
	{walk = v(0, 0, 3), speed = 1.5, steps = "default_grass_footstep", log = "to the water's edge"},
	{wait = 8},
	{fn = function() set_time(0.18, 0.27, 14) end, wait = 16, log = "dawn"},
	{walk = v(4, 0, -6), speed = 2.5, steps = "default_grass_footstep", log = "to a tree"},
	{fn = function() dig(v(9, 4, -7)) end, wait = 8, log = "breaking leaves"},
}

TIMELINE.home = {
	{fn = function()
		core.set_timeofday(0.9)
		afx_ambience.set_rain(0.7)
		bed("afx_out_rain", 0.7)
	end, wait = 22, log = "home on a rainy night"},
	{walk = v(0, 0, -4), speed = 2, steps = "default_wood_footstep", log = "to the door"},
	{wait = 6},
	{walk = v(-2, 0, 2), speed = 2, steps = "default_wood_footstep", log = "back by the bed"},
	{wait = 8},
}

TIMELINE.mineshaft = {
	{wait = 16, log = "in the mineshaft"},
	{walk = v(26, -20, 0), speed = 2.5, steps = "default_hard_footstep", log = "along the rails"},
	{walk = v(38, -20, -2), speed = 2, steps = "default_hard_footstep", log = "into the dungeon"},
	{wait = 30},
}

TIMELINE.steps = {
	{wait = 1, log = "stone"},
	{walk = v(0, 0, 12), speed = 2.2, steps = "default_hard_footstep"},
	{wait = 1, log = "wood"},
	{walk = v(0, 0, 0), speed = 2.2, steps = "default_wood_footstep"},
	{wait = 1, log = "grass"},
	{walk = v(0, 0, 12), speed = 2.2, steps = "default_grass_footstep"},
	{fn = tap, wait = 0.7}, {fn = tap, wait = 0.7}, {fn = tap, wait = 0.7},
	{fn = tap, wait = 0.7}, {fn = tap, wait = 0.7}, {fn = tap, wait = 2},
}

local function miner(pos, n, every)
	for i = 0, n - 1 do
		core.after(i * every, function()
			core.sound_play("default_dig_cracky", {pos = pos, to_player = player_name,
				gain = 1.0, max_hear_distance = 48}, true)
		end)
	end
end

TIMELINE.corner = {
	{fn = function() miner(v(3, 1, 14), 60, 0.8) end, wait = 8, log = "miner around the corner"},
	{walk = v(-2, 0, 0), speed = 2, steps = "default_hard_footstep", log = "toward the bend"},
	{walk = v(3, 0, 4), speed = 2, steps = "default_hard_footstep", log = "around it"},
	{wait = 6},
	{walk = v(-16, 0, 0), speed = 3, steps = "default_hard_footstep", log = "back away"},
	{wait = 6},
}

TIMELINE.nextdoor = {
	{fn = tap, wait = 1.2, log = "in the hut, door open"}, {fn = tap, wait = 1.2},
	{fn = tap, wait = 3},
	{fn = function()
		for y = 0, 1 do core.set_node(v(0, y, 1), {name = "afxscene:door", param2 = 0}) end
	end, wait = 2, log = "door shut"},
	{fn = tap, wait = 1.2}, {fn = tap, wait = 1.2}, {fn = tap, wait = 3},
	{fn = function()
		for y = 0, 1 do core.set_node(v(0, y, 1), {name = "air"}) end
	end, wait = 2, log = "door open"},
	{fn = tap, wait = 1.2}, {fn = tap, wait = 3},
}

local function growl(pos, n)
	for i = 0, n - 1 do
		core.after(i * 3.2, function()
			core.sound_play("afxscene_growl", {pos = pos, gain = 1.0, max_hear_distance = 48}, true)
		end)
	end
end

TIMELINE.voices = {
	{fn = function() afx_ambience.lib.player_state(player_name).next_timber = 0 end,
		wait = 10, log = "a sealed wooden hut, nothing playing"},
	{fn = function() P():set_pos(v(0, 0, 30)) end, wait = 8, log = "inside the stone cave"},
	{fn = function() P():set_pos(v(0, 0, 0)) end, wait = 3, log = "outside, open meadow"},
	{fn = function() growl(v(0, 0.5, 27), 3) end, wait = 10,
		log = "a zombie deep in the cave (source space on)"},
	{fn = function() core.settings:set("afx_source_space", "0") end, wait = 1},
	{fn = function() growl(v(0, 0.5, 27), 3) end, wait = 10, log = "the same, source space off"},
	{fn = function() core.settings:set("afx_source_space", "1") end, wait = 1},
	{fn = function() growl(v(13, -3.5, -3), 3) end, wait = 10, log = "something in the pond"},
	{fn = function() core.settings:set("afx_source_space", "0") end, wait = 1},
	{fn = function() growl(v(13, -3.5, -3), 3) end, wait = 10, log = "the pond, source space off"},
	{fn = function()
		core.settings:set("afx_source_space", "1")
		-- exercise the mixer and the report
		local ok, msg = core.registered_chatcommands.afx.func(player_name, "")
		core.log("action", "afxscene: /afx -> " .. tostring(ok))
		for _, cb in ipairs(core.registered_on_player_receive_fields) do
			cb(P(), "afx_ambience:mixer", {e_afx_reverb = "CHG:150", v_birds = "CHG:40",
				afx_ambience_follower = "true"})
		end
		core.log("action", "afxscene: mixer -> reverb " .. core.settings:get("afx_reverb") ..
			" birds " .. afx_ambience.volume("birds") ..
			" follower " .. tostring(core.settings:get_bool("afx_ambience_follower")))
		ok, msg = core.registered_chatcommands.afx_report.func(player_name, "")
		core.log("action", "afxscene: /afx_report -> " .. tostring(msg))
	end, wait = 1, log = "mixer"},
}

-- (driven by AFX_XDO: hold the left mouse button from ~4 s to the end)
TIMELINE.knock = {
	{fn = function() P():set_pos(v(-4, 0, -1)); P():set_look_horizontal(0); P():set_look_vertical(0) end,
		wait = 9, log = "punching solid rock"},
	{fn = function() P():set_pos(v(5, 0, -1)); P():set_look_horizontal(0) end,
		wait = 9, log = "punching rock with a cave behind it"},
	{fn = function() P():set_pos(v(-4, 0, -1)) end, wait = 6, log = "solid again"},
}

local function climb(x0, y0, z, steps_down)
	local seq = {}
	for i = 1, 8 do
		seq[#seq + 1] = {fn = function() P():set_pos(v(x0 + i, y0 + i, z)) end, wait = 0.55}
	end
	return seq
end

local function box(pos)
	local def = core.registered_nodes["afx_ambience:music_box"]
	def.on_rightclick(pos, core.get_node(pos), P())
end

TIMELINE.music = {}
local function add(t) for _, s in ipairs(t) do TIMELINE.music[#TIMELINE.music + 1] = s end end
add({{fn = function() P():set_pos(v(-14, 0, -2)) end, wait = 2, log = "meadow: up the note stair"}})
add(climb(-12, 0, 0))
add({{fn = function() box(v(4, 0, 4)) end, wait = 7, log = "the music box on the grass"}})
add({{fn = function() P():set_pos(v(0, -28, -6)) end, wait = 6, log = "down in the cavern (first visit: a chime)"}})
add(climb(-6, -28, 2))
add({{fn = function() box(v(6, -28, 6)) end, wait = 8, log = "the music box in the cavern"}})
-- wind: a far voice upwind, then the same downwind
local function growl_at(p) core.sound_play("afxscene_growl", {pos = p, gain = 1, max_hear_distance = 64}, true) end
add({{fn = function()
	P():set_pos(v(0, 0, -6))
	afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 1.4}
end, wait = 3, log = "meadow, a strong wind blowing east"},
	{fn = function() growl_at(v(28, 0.5, -6)) end, wait = 3, log = "a voice 28 nodes east (upwind)"},
	{fn = function() growl_at(v(28, 0.5, -6)) end, wait = 3},
	{fn = function() growl_at(v(-28, 0.5, -6)) end, wait = 3, log = "a voice 28 nodes west (downwind)"},
	{fn = function() growl_at(v(-28, 0.5, -6)) end, wait = 3},
	{fn = function() afx_ambience.wind_override = nil end, wait = 1}})

TIMELINE.walls = {
	{walk = v(-26, 0, -8), speed = 2.2, steps = "default_hard_footstep", log = "open meadow"},
	{walk = v(-26, 0, 0), speed = 2.2, steps = "default_hard_footstep"},
	{walk = v(-5, 0, 0), speed = 2.2, steps = "default_hard_footstep", log = "along a wall on the left"},
	{walk = v(13, 0, 0), speed = 2.2, steps = "default_hard_footstep", log = "between two walls"},
	{walk = v(24, 0, 0), speed = 2.2, steps = "default_hard_footstep", log = "out in the open again"},
	{wait = 1},
}

local function drop_at(p, item)
	core.add_item(vector.offset(p, 0, 3, 0), item or "basenodes:stone")
end

TIMELINE.accents = {
	{fn = function()
		afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 1.0}
		P():set_pos(v(0, 0, -10))
	end, wait = 4, log = "a calm meadow"},
	{fn = function() afx_ambience.set_rain(0.8) end, wait = 26, log = "rain coming in from the west"},
	{fn = function() P():set_pos(v(9, 0, -9)) end, wait = 7, log = "rain on the lava"},
	{fn = function() P():set_pos(v(0, 0, 0)) end, wait = 6, log = "inside the sealed hut"},
	{fn = function() core.set_node(v(0, 0, -4), {name = "air"}); core.set_node(v(0, 1, -4), {name = "air"}) end,
		wait = 5, log = "the door opens"},
	{fn = function() afx_ambience.thunder_at(v(60, 20, 30)) end, wait = 6, log = "thunder near: the window rattles"},
	{fn = function() P():set_pos(v(0, 0, -10)); afx_ambience.set_rain(0) end, wait = 22,
		log = "rain stops, moving off east"},
	{fn = function() drop_at(v(2, 0, -9)) end, wait = 1.5, log = "things land: on grass"},
	{fn = function() P():set_pos(v(-10, 0, -12)) end, wait = 1.5},
	{fn = function() drop_at(v(-10, 0, -11)) end, wait = 1.5, log = "on planks over a drop"},
	{fn = function() drop_at(v(-9, 0, -12)) end, wait = 1.5},
	{fn = function() P():set_pos(v(0, -1, 10)); core.set_node(v(1, -1, 10), {name = "basenodes:stone"}) end, wait = 1},
	{fn = function() drop_at(v(1, -1, 10)) end, wait = 1.5, log = "on stone"},
	{fn = function() afx_ambience.wind_override = nil end, wait = 1},
}

BUILD.mixer = function(b)
	b.fill(v(MIN.x, -6, MIN.z), v(MAX.x, -1, MAX.z), "basenodes:dirt_with_grass")
end
TIMELINE.mixer = {
	{fn = function() core.registered_chatcommands.afx.func(player_name, "") end, wait = 5, log = "mixer open"},
	{fn = function()
		for _, cb in ipairs(core.registered_on_player_receive_fields) do
			cb(P(), "afx_ambience:mixer", {e_afx_reverb = "CHG:170", v_birds = "VAL:100"})
		end
	end, wait = 5, log = "reverb dragged to 170%"},
}

local function dart_past(p, dir)
	local o = core.add_entity(vector.add(p, vector.multiply(dir, -18)), "afxscene:dart")
	if o then o:set_velocity(vector.multiply(dir, 30)) end
end

TIMELINE.nature = {
	{fn = function()
		core.set_timeofday(0.5)
		afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 1.2}
		P():set_pos(v(-28, 0, -28))
	end, wait = 6, log = "in the wood, a breeze"},
	{fn = function() afx_ambience.gust_now(1.3) end, wait = 9, log = "a gust comes through the trees"},
	{fn = function() afx_ambience.gust_now(1.4) end, wait = 9, log = "another"},
	{fn = function() afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 0.7}
		core.set_timeofday(0.95); P():set_pos(v(0, 0, -33)) end, wait = 12, log = "night meadow: crickets"},
	{walk = v(10, 0, -33), speed = 1.6, steps = "default_grass_footstep", log = "walking through them"},
	{wait = 8},
	{fn = function() P():set_pos(v(33, 0, -34)) end, wait = 30, log = "a frozen lake at night"},
	{fn = function() core.set_timeofday(0.5); P():set_pos(v(-24, 0, 20)) end, wait = 4,
		log = "west of a ridge, a voice beyond it"},
	{fn = function() core.sound_play("afxscene_growl", {pos = v(40, 0.5, 20), gain = 1, max_hear_distance = 128}, true) end, wait = 3},
	{fn = function() core.sound_play("afxscene_growl", {pos = v(40, 0.5, 20), gain = 1, max_hear_distance = 128}, true) end, wait = 3},
	{fn = function() P():set_pos(v(-24, 0, -20)) end, wait = 3, log = "the same voice, nothing in between"},
	{fn = function() core.sound_play("afxscene_growl", {pos = v(40, 0.5, -20), gain = 1, max_hear_distance = 128}, true) end, wait = 3},
	{fn = function() core.sound_play("afxscene_growl", {pos = v(40, 0.5, -20), gain = 1, max_hear_distance = 128}, true) end, wait = 3},
	{fn = function() P():set_pos(v(-30, 0, 10)); afx_ambience.thunder_at(v(-200, 40, 0)) end, wait = 9,
		log = "thunder rolls back off the ridge"},
	{fn = function() P():set_pos(v(-10, 0, -10)); dart_past(v(-10, 1.6, -11.5), {x = 1, y = 0, z = 0}) end,
		wait = 2, log = "something flies past"},
	{fn = function() dart_past(v(-10, 1.7, -9), {x = -1, y = 0, z = 0}) end, wait = 3},
	{fn = function() afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 1.5}
		P():set_pos(v(46, 0, 29)) end, wait = 4, log = "in the cave, gusts across its mouth"},
	{fn = function() afx_ambience.gust_now(1.5) end, wait = 10},
	{walk = v(52, 0, 22), speed = 11, steps = "default_hard_footstep", log = "a run in the quiet cave"},
	{walk = v(40, 0, 36), speed = 11, steps = "default_hard_footstep"},
	{fn = function() afx_ambience.wind_override = {dir = {x = 1, y = 0, z = 0}, wind = 0.5} end, wait = 10,
		log = "standing still, getting your breath back"},
	{fn = function() afx_ambience.wind_override = nil end, wait = 1},
}

TIMELINE.bridge = {
	{walk = v(-8, 0, 0), speed = 2, steps = "default_hard_footstep", log = "on rock"},
	{walk = v(6, 0, 0), speed = 2, steps = "default_wood_footstep", log = "on the bridge"},
	{walk = v(14, 0, 0), speed = 2, steps = "default_hard_footstep", log = "rock again"},
	{wait = 2},
}

TIMELINE.draft = {
	{fn = function() bed("afx_out_wind", 0.8); afx_ambience.gust_now(1.5) end, wait = 16,
		log = "inside, a storm outside"},
	{fn = function() afx_ambience.gust_now(1.6) end, wait = 14, log = "another gust"},
	{fn = function() core.set_node(v(4, 2, 0), {name = "afxscene:planks"}) end, wait = 10,
		log = "gap boarded up"},
}

local function fall_from(pos)
	local pl = P()
	pl:set_pos(pos)
	pl:set_physics_override({gravity = 1, speed = 0, jump = 0})
end

TIMELINE.details = {
	{walk = v(-6, 0, 0), speed = 3, steps = "default_grass_footstep", log = "through tall grass"},
	{walk = v(2, 0, 6), speed = 2.5, steps = "default_grass_footstep", log = "to the porch"},
	{fn = function() afx_ambience.gust_now(1.6) end, wait = 12, log = "wind chime in a gust"},
	{fn = function() core.set_timeofday(0.245) end, wait = 12, log = "dawn: the bell"},
	{fn = function() core.set_timeofday(0.5); afx_ambience.set_rain(0.7);
		bed("afx_out_rain", 0.6) end, wait = 10, log = "rain"},
	{fn = function() afx_ambience.set_rain(0) end, wait = 1, log = "rain stops"},
	{walk = v(-8, 0, 14), speed = 3, steps = "default_grass_footstep", log = "wet steps"},
	{fn = function() fall_from(v(-25, 60, 25)) end, wait = 5, log = "falling"},
	{fn = function()
		-- devtest doesn't drown you; let the breath run out by hand
		for i = 0, 9 do
			core.after(i * 1.2, function() local pl = P(); if pl then pl:set_breath(10 - i) end end)
		end
	end, wait = 14, log = "under water"},
	{fn = function()
		local pl = P()
		pl:set_physics_override({gravity = 0, speed = 0, jump = 0})
		pl:set_pos(v(20, -30, -20))
		pl:get_inventory():set_stack("main", 1, "afxscene:pocket_clock")
		afx_ambience.lib.player_state(player_name).next_settle = 0
	end, wait = 14, log = "the cave under the lake, a clock in hand"},
}

TIMELINE.cinema = {
	{wait = 48, log = "a meadow at noon (the score starts after a while)"},
	{fn = function() core.set_timeofday(0.9) end, wait = 26, log = "night"},
	{fn = function()
		core.sound_play("tnt_explode", {pos = vector.offset(P():get_pos(), 5, 0, 3),
			gain = 1.0, max_hear_distance = 64}, true)
	end, wait = 9, log = "an explosion close by"},
	{fn = function() P():set_hp(3) end, wait = 10, log = "badly hurt"},
	{fn = function()
		local pl = P()
		pl:set_hp(20)
		pl:set_pos(v(0, -30, 0))
		local st = afx_ambience.lib.player_state(player_name)
		st.next_follow = 0
	end, wait = 2, log = "deep in the dark"},
	{walk = v(9, -30, 0), speed = 2, steps = "default_hard_footstep"},
	{wait = 2},
	{walk = v(-8, -30, 2), speed = 2, steps = "default_hard_footstep"},
	{wait = 3},
	{walk = v(0, -30, -8), speed = 2, steps = "default_hard_footstep"},
	{wait = 20, log = "standing still in the dark"},
}

local START = {
	cinema = v(0, 0, 0), voices = v(-24, 0, -20), knock = v(-4, 0, -1), music = v(-14, 0, -2), walls = v(-26, 0, -14), accents = v(0, 0, -10), mixer = v(0, 0, 0), nature = v(-28, 0, -28),
	details = v(-24, 0, 0),
	corner = v(-16, 0, 0), nextdoor = v(0, 0, -4), bridge = v(-16, 0, 0), draft = v(0, 0, 0),
	steps = v(0, 0, 0),
	storm = v(-14, 0, -2), shore = v(0, 0, -6), home = v(-2, 0, 2), mineshaft = v(-20, -20, 0),
	descent = v(0, 0, 1), meadow = v(0, 0, 0), beach = v(4, 0, -2), sky_island = v(0, 80, -2),
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
