-- Mineclonia test scene for util/fx/test (singlenode world).
-- Settings: fxmcl_time (0..1, default 0 = midnight), fxmcl_view (lights).
-- Every node is checked against the registry; misses are logged, not fatal.

local S = core.settings

local function place(pos, name, param2)
	if not core.registered_nodes[name] then
		core.log("warning", "[fxmcl] node not registered: " .. name)
		return
	end
	core.set_node(pos, {name = name, param2 = param2 or 0})
end

local function box(x0, y0, z0, x1, y1, z1, name)
	for x = x0, x1 do
		for y = y0, y1 do
			for z = z0, z1 do
				place({x = x, y = y, z = z}, name)
			end
		end
	end
end

local function build()
	-- room: stone floor, cobble walls, deepslate ceiling (no sky light)
	box(-12, -1, -6, 12, -1, 12, "mcl_core:stone")
	box(-12, 7, -6, 12, 7, 12, "mcl_deepslate:deepslate")
	box(-12, 0, 12, 12, 6, 12, "mcl_core:cobble")
	box(-12, 0, -6, -12, 6, 12, "mcl_core:cobble")
	box(12, 0, -6, 12, 6, 12, "mcl_core:cobble")

	-- front row (z = 3): light sources, every other slot
	local lights = {
		"mcl_torches:torch", "mcl_lanterns:lantern_floor", "mcl_nether:glowstone",
		"mcl_ocean:sea_lantern", "mcl_farming:pumpkin_face_light", "mcl_furnaces:furnace_active",
		"mcl_end:end_rod", "mcl_campfires:campfire_lit", "mcl_redstone_lamp:lamp_on",
		"mcl_crimson:shroomlight", "mcl_blackstone:soul_torch",
	}
	for i, name in ipairs(lights) do
		place({x = -11 + i * 2, y = 0, z = 3}, name, 0) -- fronts face the camera
	end
	-- second row (z = 6): their unlit twins and lookalikes; must not glow.
	-- A lit jack o'lantern turned sideways: its pumpkin side must not glow.
	local unlit = {
		[5] = "mcl_farming:pumpkin", [6] = "mcl_furnaces:furnace",
		[9] = "mcl_redstone_lamp:lamp_off", [3] = "mcl_nether:netherrack",
		[7] = "mcl_farming:pumpkin_face_light",
	}
	for i, name in pairs(unlit) do
		place({x = -11 + i * 2, y = 0, z = 6}, name, name:find("face_light") and 3 or 0) -- face away
	end

	-- back wall (z = 11): ores and metals
	local wall = {
		"mcl_core:stone_with_gold", "mcl_core:stone_with_diamond", "mcl_core:stone_with_iron",
		"mcl_core:goldblock", "mcl_core:ironblock", "mcl_core:diamondblock",
		"mcl_core:stone_with_emerald", "mcl_core:obsidian", "mcl_core:crying_obsidian",
	}
	for i, name in ipairs(wall) do
		box(-10 + i * 2, 1, 11, -9 + i * 2, 2, 11, name)
	end

	-- glass (z = 8): clear, red, light blue; water and lava pools
	box(-9, 0, 8, -7, 2, 8, "mcl_core:glass")
	box(-5, 0, 8, -3, 2, 8, "mcl_core:glass_red")
	box(-1, 0, 8, 1, 2, 8, "mcl_core:glass_light_blue")
	box(4, -1, 7, 7, -1, 9, "mcl_core:water_source")
	box(9, -1, 7, 10, -1, 9, "mcl_core:lava_source")
end

core.register_on_joinplayer(function(player)
	S:set("time_speed", "0")
	core.set_timeofday(tonumber(S:get("fxmcl_time") or "") or 0.0)
	player:set_physics_override({gravity = 0, speed = 0})
	player:hud_set_flags({hotbar = false, healthbar = false, crosshair = false,
		wielditem = false, breathbar = false, minimap = false, basic_debug = false})
	if rawget(_G, "mcl_weather") and mcl_weather.change_weather then
		pcall(mcl_weather.change_weather, "none")
	end
	-- Mineclonia's spawn logic may move the player after join; pin the
	-- camera again right before signalling the test harness
	local function pin()
		if not player:is_valid() then
			return
		end
		player:set_pos({x = 0, y = -0.5, z = -2})
		player:set_look_horizontal(0)
		player:set_look_vertical(0.12)
	end
	core.after(0.5, function()
		build()
		pin()
		core.after(1.0, function()
			pin()
			core.after(0.2, function()
				pin()
				S:set_bool("fx_test_ready", true)
			end)
		end)
	end)
end)
