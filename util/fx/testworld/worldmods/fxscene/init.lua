-- Deterministic night scene for checking the fx renderer.
-- Simple solid-colored lamp blocks: the texture is the light color.
local function simple_lamp(name, color)
	core.register_node("fxscene:" .. name, {
		description = name,
		tiles = {"[fill:16x16:#" .. color},
		light_source = 14,
		groups = {dig_immediate = 3},
	})
end
simple_lamp("red", "ff3020")
simple_lamp("blue", "3060ff")
simple_lamp("green", "30ff60")

core.register_on_joinplayer(function(player)
	core.set_timeofday(0.0)
	core.settings:set("time_speed", "0")
	player:hud_set_flags({hotbar = false, healthbar = false, crosshair = false,
		wielditem = false, breathbar = false, minimap = false, basic_debug = false})
	core.after(0.5, function()
		local stone = "basenodes:stone"
		-- floor and back wall
		for x = -12, 12 do
			for z = -2, 18 do
				core.set_node({x = x, y = -1, z = z}, {name = stone})
				core.set_node({x = x, y = 8, z = z}, {name = stone}) -- ceiling, blocks sky
			end
			for y = 0, 7 do
				core.set_node({x = x, y = y, z = 18}, {name = "basenodes:cobble"})
			end
		end
		for z = -2, 18 do
			for y = 0, 7 do
				core.set_node({x = -12, y = y, z = z}, {name = "basenodes:cobble"})
				core.set_node({x = 12, y = y, z = z}, {name = "basenodes:cobble"})
			end
		end
		-- three colored lamps against the back wall
		core.set_node({x = -6, y = 1, z = 16}, {name = "fxscene:red"})
		core.set_node({x = 0, y = 1, z = 16}, {name = "fxscene:blue"})
		core.set_node({x = 6, y = 1, z = 16}, {name = "fxscene:green"})
		-- a lava pool in the front left
		for x = -10, -7 do
			for z = 4, 7 do
				core.set_node({x = x, y = -1, z = z}, {name = "basenodes:lava_source"})
			end
		end
		-- a white light (devtest light14) front right
		core.set_node({x = 9, y = 0, z = 6}, {name = "testnodes:light14"})
		player:set_pos({x = 0, y = 2.5, z = -1})
		player:set_look_horizontal(0)
		player:set_look_vertical(0.25)
	end)
end)
