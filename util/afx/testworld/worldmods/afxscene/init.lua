-- Deterministic tour for checking the afx fork (environmental audio).
--
-- Builds six stations on a flat stone plain at noon and walks the player
-- through them, 10 s each, facing north (+Z):
--
--   1 open   open field
--   2 cave   13x7x13 hollow deep inside stone
--   3 tunnel 20 nodes into a 3x3 tunnel whose mouth is east (to the right)
--   4 hut    inside a 7x5x7 wooden hut
--   5 cliff  30-node stone cliff 20 nodes north
--   6 water  under water
--
-- A looping outdoor bed (afx_out_testwind) plays throughout; every 2.5 s a
-- local click plays (room reverb) and a positional click 8 nodes north
-- (occlusion). With afx_debug the client logs its acoustic state every 2 s.

local STATIONS = {
	{name = "open",   pos = {x = 0,   y = 0,  z = 0}},
	{name = "cave",   pos = {x = 50,  y = 0,  z = 0}},
	{name = "tunnel", pos = {x = 92,  y = 0,  z = 0}},
	{name = "hut",    pos = {x = 140, y = 0,  z = 0}},
	{name = "cliff",  pos = {x = 180, y = 0,  z = 0}},
	{name = "water",  pos = {x = 220, y = -4, z = 0}},
}
local DWELL = tonumber(core.settings:get("afxscene_dwell") or "") or 10

local MIN = {x = -40, y = -10, z = -48}
local MAX = {x = 260, y = 40, z = 48}

local function build()
	local vm = core.get_voxel_manip()
	local emin, emax = vm:read_from_map(MIN, MAX)
	local area = VoxelArea(emin, emax)
	local data = vm:get_data()
	local c_air = core.get_content_id("air")
	local c_stone = core.get_content_id("basenodes:stone")
	local c_tree = core.get_content_id("basenodes:tree")
	local c_water = core.get_content_id("basenodes:water_source")

	local function fill(a, b, c)
		for z = a.z, b.z do
			for y = a.y, b.y do
				local vi = area:index(a.x, y, z)
				for _ = a.x, b.x do
					data[vi] = c
					vi = vi + 1
				end
			end
		end
	end
	local function v(x, y, z) return {x = x, y = y, z = z} end

	fill(v(MIN.x, MIN.y, MIN.z), v(MAX.x, MAX.y, MAX.z), c_air)
	fill(v(MIN.x, -10, MIN.z), v(MAX.x, -1, MAX.z), c_stone) -- the plain

	-- 2 cave
	fill(v(30, 0, -20), v(70, 25, 20), c_stone)
	fill(v(44, 0, -6), v(56, 6, 6), c_air)

	-- 3 tunnel, mouth at x=110 facing east
	fill(v(75, 0, -25), v(110, 22, 25), c_stone)
	fill(v(88, 0, -1), v(110, 2, 1), c_air)

	-- 4 hut
	fill(v(136, 0, -4), v(144, 5, 4), c_tree)
	fill(v(137, 0, -3), v(143, 4, 3), c_air)

	-- 5 cliff north
	fill(v(150, 0, 20), v(210, 30, 30), c_stone)

	-- 6 pool
	fill(v(212, -8, -8), v(228, -1, 8), c_water)

	vm:set_data(data)
	vm:calc_lighting()
	vm:write_to_map(true)
end

local function tour(player, i)
	if not player or not player:is_player() then
		return
	end
	local st = STATIONS[i]
	if not st then
		core.log("action", "afxscene: done")
		core.request_shutdown("afxscene done", false, 1)
		return
	end
	core.log("action", "afxscene: station " .. st.name)
	player:set_pos(st.pos)
	player:set_look_horizontal(0)
	player:set_look_vertical(0)
	core.after(DWELL, tour, player, i + 1)
end

local function clicks(player)
	if not player or not player:is_player() then
		return
	end
	local name = player:get_player_name()
	core.sound_play("afx_test_click", {to_player = name, gain = 0.8}, true)
	local p = vector.add(player:get_pos(), {x = 0, y = 1, z = 8})
	core.after(1.2, function()
		core.sound_play("afx_test_click", {pos = p, gain = 1.0, max_hear_distance = 64}, true)
	end)
	core.after(2.5, clicks, player)
end

core.register_on_joinplayer(function(player)
	core.set_timeofday(0.5)
	core.settings:set("time_speed", "0")
	player:set_physics_override({gravity = 0, speed = 0, jump = 0})
	core.emerge_area(MIN, MAX, function(_, _, remaining)
		if remaining > 0 then
			return
		end
		build()
		core.log("action", "afxscene: built")
		core.sound_play("afx_out_testwind", {to_player = player:get_player_name(),
			loop = true, gain = 0.6})
		core.after(2, tour, player, 1)
		core.after(2.3, clicks, player)
	end)
end)
