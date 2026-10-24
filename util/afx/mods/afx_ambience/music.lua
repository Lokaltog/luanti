-- Music that lives in the world: note stones that sing when you step on
-- them (build a staircase that plays a scale), and a music box that plays
-- its own little tune. They're positional sounds, so the engine rings them
-- in the room they're in: bright in a cave, close and warm at home.
--
-- And a soft chime the first time you find a new kind of place.

local A = afx_ambience
local L = A.lib

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

--[[ First discoveries: a soft rising chime, once ever per kind of place ]]

local function seen(player)
	local s = player:get_meta():get_string("afx_seen")
	return s ~= "" and core.deserialize(s) or {}
end

local function remember(player, t)
	player:get_meta():set_string("afx_seen", core.serialize(t))
end

local function chime(pname, big)
	local degs = big and {1, 3, 4, 6} or {3, 4, 6}
	for n, deg in ipairs(degs) do
		core.after((n - 1) * 0.32, function()
			A.play("afx_dry_score_bell", {to_player = pname, gain = 0.22,
				pitch = semis(SCALE[deg] + 12)}, true)
		end)
	end
	core.after(#degs * 0.32, function()
		A.play("afx_dry_score_bowl", {to_player = pname, gain = 0.2, pitch = semis(12)}, true)
	end)
end

-- What kind of place: a few broad kinds rather than every biome variant
local function kinds(ctx)
	local out = {}
	if ctx.dimension ~= "overworld" then
		out[#out + 1] = ctx.dimension
		return out
	end
	local bd = core.get_biome_data(ctx.ppos)
	local b = bd and core.get_biome_name(bd.biome) or ""
	b = b:gsub("_deep_ocean$", ""):gsub("_ocean$", ""):gsub("_underground$", "")
		:gsub("_beach$", ""):gsub("_shore$", "")
	if ctx.under_sky and b ~= "" then out[#out + 1] = "biome:" .. b:lower() end
	local h = ctx.hearing
	if ctx.underground and h and (h.decay or 0) > 1.6 and (h.mfp or 0) > 6 then
		out[#out + 1] = "cavern"
	end
	if ctx.under_sky and ctx.ppos.y > 60 then out[#out + 1] = "sky" end
	if ctx.underwater then out[#out + 1] = "underwater" end
	return out
end

A.register_layer(function(ctx)
	local st = ctx.st
	local now = ctx.now
	if now < (st.next_discovery or 0) then return end
	if (ctx.hearing and (ctx.hearing.tension or 0) > 0.6) then return end -- not mid-fright
	local list = kinds(ctx)
	if #list == 0 then return end
	-- stay a moment before it counts
	st.dwell = st.dwell or {}
	local mem
	for _, k in ipairs(list) do
		st.dwell[k] = (st.dwell[k] or 0) + 1
		if st.dwell[k] >= 4 then
			mem = mem or seen(ctx.player)
			if not mem[k] then
				mem[k] = os.time()
				remember(ctx.player, mem)
				st.next_discovery = now + 20
				core.log("action", "afx_ambience: " .. ctx.pname .. " discovered " .. k)
				chime(ctx.pname, k == "cavern" or k == "sky" or not k:find("^biome:"))
				break
			end
		end
	end
	for k in pairs(st.dwell) do
		local here = false
		for _, l in ipairs(list) do if l == k then here = true end end
		if not here then st.dwell[k] = nil end
	end
end)

A.add_report(function()
	return {"music: note stones x" .. #SCALE .. ", music box, discovery chimes"}
end)
