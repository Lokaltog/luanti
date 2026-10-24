-- Things to build that sound: a wind chime (the client's ambience rings it
-- in the wind), note stones and a music box.

local A = afx_ambience

-- Pentatonic: whatever the wind plays, it's in tune
local SCALE = {1, 9 / 8, 5 / 4, 3 / 2, 5 / 3, 2, 9 / 4}

core.register_node("afx_ambience:wind_chime", {
	description = "Wind Chime",
	drawtype = "plantlike",
	tiles = {"afx_ambience_wind_chime.png"},
	inventory_image = "afx_ambience_wind_chime.png",
	wield_image = "afx_ambience_wind_chime.png",
	paramtype = "light",
	sunlight_propagates = true,
	walkable = false,
	selection_box = {type = "fixed", fixed = {-0.3, -0.3, -0.3, 0.3, 0.5, 0.3}},
	groups = {oddly_breakable_by_hand = 3, handy = 1, dig_immediate = 3, attached_node = 4,
		deco_block = 1},
	_mcl_hardness = 0.3,
	on_punch = function(pos, node, puncher)
		-- a tap rings it
		for i = 0, 2 do
			core.after(i * 0.18, function()
				A.play("afx_bowl", {pos = pos, gain = 0.35,
					pitch = 2 * SCALE[math.random(#SCALE)], max_hear_distance = 24}, true)
			end)
		end
	end,
})

core.register_on_mods_loaded(function()
	local nugget = core.registered_items["mcl_core:iron_nugget"] and "mcl_core:iron_nugget" or
		core.registered_items["default:steel_ingot"] and "default:steel_ingot"
	local string = core.registered_items["mcl_mobitems:string"] and "mcl_mobitems:string" or
		core.registered_items["farming:string"] and "farming:string"
	local stick = core.registered_items["mcl_core:stick"] and "mcl_core:stick" or
		core.registered_items["default:stick"] and "default:stick"
	if nugget and string and stick then
		core.register_craft({output = "afx_ambience:wind_chime", recipe = {
			{"", stick, ""}, {string, string, string}, {nugget, nugget, nugget}}})
	end
end)



-- C major pentatonic over two octaves, from the C4 samples
local SCALE = {0, 2, 4, 7, 9, 12, 14, 16}
local COLORS = {"#e04040", "#e08a30", "#e0d040", "#50b050", "#40a0d0", "#5060d0", "#9050c0", "#d060a0"}
local NAMES = {"C", "D", "E", "G", "A", "high C", "high D", "high E"}
local function semis(s) return 2 ^ (s / 12) end

local function play_note(pos, i, instrument, gain)
	A.play(instrument or "afx_note_bell", {pos = pos, gain = gain or 0.5,
		pitch = semis(SCALE[i]), max_hear_distance = 32}, true)
end

--[[ Note stones ]]

local stone_groups = {cracky = 3, pickaxey = 1, building_block = 1, afx_note_stone = 1}

for i = 1, #SCALE do
	local name = "afx_ambience:note_stone_" .. i
	core.register_node(name, {
		description = "Note Stone (" .. NAMES[i] .. ")\nSings when stepped on. Punch to hear, right-click for the next note.",
		tiles = {"[fill:16x16:" .. COLORS[i] .. "^[fill:12x12:2,2:#ffffff30"},
		is_ground_content = false,
		groups = (function()
			local g = table.copy(stone_groups)
			if i > 1 then g.not_in_creative_inventory = 1 end
			return g
		end)(),
		drop = "afx_ambience:note_stone_1",
		_mcl_hardness = 0.8,
		_mcl_blast_resistance = 2,
		sounds = {footstep = {name = "", gain = 0}},
		on_punch = function(pos) play_note(pos, i) end,
		on_rightclick = function(pos, node, clicker)
			local nexti = i % #SCALE + 1
			core.swap_node(pos, {name = "afx_ambience:note_stone_" .. nexti})
			play_note(pos, nexti)
		end,
	})
end

-- Stepping on one: a note, once per step onto it
local on_stone = {}
A.register_tick(function(player)
	local name = player:get_player_name()
	local below = vector.round(vector.offset(player:get_pos(), 0, -0.6, 0))
	local node = core.get_node_or_nil(below)
	local i = node and tonumber(node.name:match("^afx_ambience:note_stone_(%d)$"))
	local key = i and core.hash_node_position(below)
	if key and key ~= on_stone[name] then
		play_note(below, i, "afx_note_bell", 0.45)
	end
	on_stone[name] = key
end)
core.register_on_leaveplayer(function(player) on_stone[player:get_player_name()] = nil end)

--[[ Music box: its own tune, from where it sits ]]

local function tune_for(pos)
	-- the same box always plays the same tune
	local seed = core.hash_node_position(pos) % 2147483647
	local rng = PcgRandom(seed)
	local notes, deg = {}, rng:next(1, 4)
	for n = 1, 8 do
		deg = math.max(1, math.min(#SCALE, deg + rng:next(-2, 2)))
		notes[n] = deg
	end
	notes[8] = 1 -- home at the end
	return notes
end

local playing = {}

local function play_box(pos)
	local key = core.hash_node_position(pos)
	if playing[key] then return end
	playing[key] = true
	local notes = tune_for(pos)
	local t = 0
	for n, deg in ipairs(notes) do
		core.after(t, function()
			if core.get_node(pos).name ~= "afx_ambience:music_box" then return end
			play_note(pos, deg, "afx_note_piano", 0.45)
			if n == 1 or n == 5 then
				A.play("afx_note_bowl", {pos = pos, gain = 0.18, pitch = semis(SCALE[deg] - 12),
					max_hear_distance = 24}, true)
			end
		end)
		t = t + (n % 4 == 0 and 0.9 or 0.45)
	end
	core.after(t + 1, function() playing[key] = nil end)
end

core.register_node("afx_ambience:music_box", {
	description = "Music Box\nRight-click: it plays its own little tune",
	tiles = {"[fill:16x16:#8a5a30^[fill:12x2:2,3:#e0c070", "[fill:16x16:#6a4220",
		"[fill:16x16:#7a4a28^[fill:4x4:6,6:#e0c070"},
	drawtype = "nodebox",
	paramtype = "light",
	paramtype2 = "facedir",
	node_box = {type = "fixed", fixed = {-0.35, -0.5, -0.25, 0.35, -0.05, 0.25}},
	groups = {choppy = 3, axey = 1, handy = 1, oddly_breakable_by_hand = 2, deco_block = 1},
	_mcl_hardness = 0.5,
	on_rightclick = function(pos) play_box(pos) end,
	on_punch = function(pos) play_box(pos) end,
	-- a redstone/mesecons pulse plays it too, where there is one
	mesecons = {effector = {action_on = function(pos) play_box(pos) end}},
})

core.register_on_mods_loaded(function()
	local function item(...)
		for _, n in ipairs({...}) do if core.registered_items[n] then return n end end
	end
	local stone = item("mcl_core:stone", "default:stone")
	local plank = item("mcl_core:wood", "default:wood")
	local iron = item("mcl_core:iron_nugget", "default:steel_ingot")
	local block = item("mesecons_noteblock:noteblock", "mcl_noteblock:noteblock")
	if stone and iron then
		core.register_craft({output = "afx_ambience:note_stone_1 4", recipe = {
			{stone, stone}, {stone, iron}}})
	end
	if plank and iron then
		core.register_craft({output = "afx_ambience:music_box", recipe = {
			{plank, iron, plank}, {plank, block or iron, plank}}})
	end
end)

