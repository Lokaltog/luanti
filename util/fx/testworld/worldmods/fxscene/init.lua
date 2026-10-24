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

-- Glass variants: the fx renderer picks patterns from the names.
local function glass(name, tex)
	core.register_node("fxscene:" .. name, {
		description = name,
		drawtype = "glasslike",
		tiles = {tex},
		use_texture_alpha = "blend",
		paramtype = "light",
		sunlight_propagates = true,
		groups = {dig_immediate = 3},
	})
end
-- thin frame, clear center
-- [fill blits with alpha, so build the frame from four strips over a clear fill
local frame = "[fill:16x16:#00000000^[fill:16x1:0,0:#c8d8e0^[fill:16x1:0,15:#c8d8e0" ..
	"^[fill:1x16:0,0:#c8d8e0^[fill:1x16:15,0:#c8d8e0"
glass("glass_clear", frame)
glass("glass_fluted", frame)
glass("glass_frosted", "[fill:16x16:#e0f0ff30^[fill:16x1:0,0:#c8d8e0")
glass("glass_red", "[fill:16x16:#ff304070")

-- M5 material test blocks (maps in util/fx/testpack)
for _, name in ipairs({"gold_ore", "gold_block"}) do
	core.register_node("fxscene:" .. name, {
		description = name,
		tiles = {"fxscene_" .. name .. ".png"},
		groups = {dig_immediate = 3},
	})
end

-- tint-aware glow: devtest's light14 glyph (magenta _e map in the test pack)
-- on a palette node; index 0 tints it red, index 1 green
core.register_node("fxscene:tinted_glow", {
	description = "tinted glow",
	tiles = {"testnodes_light_14.png"},
	paramtype2 = "color",
	palette = "fxscene_palette.png",
	groups = {dig_immediate = 3},
})

-- entity with an authored emissive companion (fxscene_mob_e.png in the test pack)
core.register_entity("fxscene:mob", {
	initial_properties = {
		visual = "cube",
		textures = {"fxscene_mob.png", "fxscene_mob.png", "fxscene_mob.png",
			"fxscene_mob.png", "fxscene_mob.png", "fxscene_mob.png"},
		physical = false,
		visual_size = {x = 0.8, y = 0.8, z = 0.8},
	},
})

-- fxscene_time: time of day 0..1 (default midnight)
-- fxscene_view: "room" (default) or "underwater"
core.register_on_joinplayer(function(player)
	core.set_timeofday(tonumber(core.settings:get("fxscene_time") or "") or 0.0)
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
		-- glass wall in front of the lamps: clear, fluted, frosted, red
		local kinds = {"glass_clear", "glass_fluted", "glass_frosted", "glass_red"}
		for i, kind in ipairs(kinds) do
			local x0 = -10 + (i - 1) * 5
			for x = x0, x0 + 3 do
				for y = 0, 3 do
					core.set_node({x = x, y = y, z = 13}, {name = "fxscene:" .. kind})
				end
			end
		end
		-- a water pool in the floor, 5 deep, flush with the floor
		for x = -3, 4 do
			for z = 2, 9 do
				for y = -6, -1 do
					local edge = x == -3 or x == 4 or z == 2 or z == 9 or y == -6
					core.set_node({x = x, y = y, z = z},
						{name = edge and "basenodes:stone" or "basenodes:water_source"})
				end
				-- keep the top edge open so the surface is level with the floor
				if not (x == -3 or x == 4 or z == 2 or z == 9) then
					core.set_node({x = x, y = -1, z = z}, {name = "basenodes:water_source"})
				end
			end
		end
		-- a few stones on the pool floor for caustics to land on
		core.set_node({x = 0, y = -5, z = 6}, {name = "basenodes:cobble"})
		core.set_node({x = 2, y = -5, z = 7}, {name = "basenodes:cobble"})
		core.set_node({x = 2, y = -4, z = 7}, {name = "basenodes:cobble"})
		-- open the ceiling above the pool so daylight reaches the water (and
		-- for the sky view, at any time)
		if (tonumber(core.settings:get("fxscene_time") or "") or 0) > 0.2
				or core.settings:get("fxscene_view") == "sky" then
			for x = -12, 12 do
				for z = -2, 18 do
					core.remove_node({x = x, y = 8, z = z})
				end
			end
		end
		-- M4 leak test: a full-height wall between the lava and a white light.
		-- With T1 the white side picks up orange through the wall; with the
		-- light volume it must stay white.
		for z = 2, 10 do
			for y = 0, 7 do
				core.set_node({x = -6, y = y, z = z}, {name = "basenodes:stone"})
			end
		end
		core.set_node({x = -4, y = 0, z = 4}, {name = "testnodes:light14"})
		-- M4 tint test: light14 boxed in red stained glass; the floor around
		-- it should turn red
		for x = 8, 10 do
			for y = 0, 2 do
				for z = 5, 7 do
					if not (x == 9 and y == 0 and z == 6) then
						core.set_node({x = x, y = y, z = z}, {name = "fxscene:glass_red"})
					end
				end
			end
		end
		-- dropped items (entities) in front of the lamps: colored tint + rim light
		for i, x in ipairs({-6, 0, 6}) do
			core.add_item({x = x, y = 0.5, z = 11}, "basenodes:cobble")
		end
		core.add_entity({x = 3, y = 0.4, z = 9}, "fxscene:mob")
		-- M5: a row of material samples, only in "materials" view
		local samples = {"basenodes:stone", "basenodes:cobble", "fxscene:gold_ore",
			"fxscene:gold_block", "basenodes:ice"}
		for i, name in ipairs(core.settings:get("fxscene_view") == "materials" and samples or {}) do
			for y = 0, 1 do
				core.set_node({x = -5 + (i - 1) * 2, y = y, z = 2}, {name = name})
				core.set_node({x = -4 + (i - 1) * 2, y = y, z = 2}, {name = name})
			end
		end
		if core.settings:get("fxscene_view") == "materials" then
			core.set_node({x = -1, y = 2, z = 2}, {name = "fxscene:tinted_glow", param2 = 0})
			core.set_node({x = 1, y = 2, z = 2}, {name = "fxscene:tinted_glow", param2 = 1})
		end
		-- fx_ambience test props, only in "particles" view
		if core.settings:get("fxscene_view") == "particles" then
			for x = -3, -1 do
				core.set_node({x = x, y = -1, z = 4}, {name = "basenodes:lava_source"})
			end
			for x = 1, 3 do
				core.set_node({x = x, y = 3, z = 5}, {name = "basenodes:leaves"})
			end
			-- plants along the right side (GPU particles: fireflies at night
			-- under the open sky, fxscene_open_sky)
			for x = 6, 10 do
				for z = 8, 11 do
					core.set_node({x = x, y = 0, z = z}, {name = "testnodes:plantlike"})
				end
			end
		end
		if core.settings:get_bool("fxscene_open_sky", false) then
			for x = -12, 12 do
				for z = -2, 18 do
					core.remove_node({x = x, y = 8, z = z})
				end
			end
		end
		local view = core.settings:get("fxscene_view")
		if view == "hand" then
			-- the wielded item against a wall 1.5 nodes away (R3: no AO halo)
			player:hud_set_flags({wielditem = true})
			player:get_inventory():set_stack("main", 1,
				core.settings:get("fxscene_wield") or "basenodes:cobble")
			player:set_pos({x = -4.5, y = 0, z = 6})
			player:set_look_horizontal(math.pi / 2)
			player:set_look_vertical(0.1)
		elseif view == "underwater_up" then
			-- under water, looking up at the surface (R3: absorption stops there)
			player:set_pos({x = 0.5, y = -5.4, z = 4.0})
			player:set_look_horizontal(0)
			player:set_look_vertical(-0.9)
		elseif view == "sky" then
			-- looking up at the open sky (aurora, End sky, scattering)
			player:set_pos({x = 0, y = 0, z = 4})
			player:set_look_horizontal(0)
			player:set_look_vertical(-1.45)
		elseif view == "materials" then
			player:set_pos({x = 0, y = 0.6, z = -2.2})
			player:set_look_horizontal(0)
			player:set_look_vertical(0.12)
		elseif core.settings:get("fxscene_view") == "underwater" then
			player:set_pos({x = 0.5, y = -5.4, z = 3.0})
			player:set_look_horizontal(0)
			player:set_look_vertical(0.05)
		else
			player:set_pos({x = 0, y = 2.5, z = -1})
			player:set_look_horizontal(0)
			player:set_look_vertical(0.25)
		end
		-- fx test mode: the scene is built (singleplayer shares settings with
		-- the client, which waits for this before it starts counting frames)
		core.settings:set_bool("fx_test_ready", true)
	end)
end)
