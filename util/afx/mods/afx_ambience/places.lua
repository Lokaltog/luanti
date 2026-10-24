-- Places: the sounds of a home, old timbers creaking in a mineshaft, chains
-- in a dungeon, and the Nether and the End sounding like other worlds.

local A = afx_ambience
local L = A.lib

--[[ Node lists, by name, filled when every mod is loaded ]]

local HOME, RAILS, TIMBER, SPAWNERS, MOSSY = {}, {}, {}, {}, {}
core.register_on_mods_loaded(function()
	for name, def in pairs(core.registered_nodes) do
		local g = def.groups or {}
		if (g.bed or 0) > 0 or (g.container or 0) > 0 or name:find("bookshelf") or
				name:find("crafting_table") or name:find("furnace") or name:find("barrel") or
				name:find("chest") or name:find("lantern") then
			HOME[#HOME + 1] = name
		end
		if (g.rail or 0) > 0 or name:find(":rail") then
			RAILS[#RAILS + 1] = name
		end
		if (g.fence or 0) > 0 and ((g.wood or 0) > 0 or name:find("wood") or name:find("oak")) or
				name:find("fence") and not name:find("nether") then
			TIMBER[#TIMBER + 1] = name
		end
		if name:find("spawner") then
			SPAWNERS[#SPAWNERS + 1] = name
		end
		if name:find("mossy") and (name:find("cobble") or name:find("stone")) then
			MOSSY[#MOSSY + 1] = name
		end
	end
end)

local function find(ctx, r, names)
	if #names == 0 then return {} end
	return core.find_nodes_in_area(vector.subtract(ctx.ppos, r), vector.add(ctx.ppos, r), names)
end

--[[ Home: a ticking clock, a purring cat ]]

local function clock_nearby(ctx)
	for _, obj in ipairs(core.get_objects_inside_radius(ctx.ppos, 8)) do
		local ent = obj:get_luaentity()
		if ent and ent.name and ent.name:find("frame") then
			-- item frames keep their item in a field; games name it differently
			for _, k in ipairs({"_item", "item", "_itemstring"}) do
				local v = ent[k]
				if type(v) == "string" and v:find("clock") then return obj end
			end
		end
	end
	for _, p in ipairs(find(ctx, 8, {"group:clock"})) do return p end
end

local function home_layer(ctx, wanted)
	if ctx.under_sky or ctx.dimension ~= "overworld" then return end
	local home = find(ctx, 8, HOME)
	if #home < 3 then return end

	-- A clock on the wall (item frame or a clock node)
	local clock = clock_nearby(ctx)
	if clock then
		if type(clock) == "table" and clock.x then
			wanted.h_clock = {sound = "afx_clock", pos = clock, gain = 0.3, range = 10}
		else
			wanted.h_clock = {sound = "afx_clock", object = clock, gain = 0.3, range = 10}
		end
	end
end

local function cats(ctx, wanted)
	for _, obj in ipairs(core.get_objects_inside_radius(ctx.ppos, 4)) do
		local ent = obj:get_luaentity()
		if ent and ent.name and (ent.name:find(":cat") or ent.name:find("kitten")) then
			-- a tamed or sitting cat close by purrs
			if ent.tamed or ent.owner or ent.order == "sit" or ent._sitting then
				wanted.purr = {sound = "afx_purr", object = obj, gain = 0.5, range = 6}
				return
			end
		end
	end
end

--[[ Underground structures ]]

local function structures(ctx, wanted)
	if not ctx.underground then return end
	local st, now = ctx.st, ctx.now

	-- A mineshaft: rails and timber framing around you
	local rails = find(ctx, 10, RAILS)
	local timber = #rails >= 3 and find(ctx, 10, TIMBER) or {}
	if #timber >= 3 and now >= (st.next_creak or 0) then
		st.next_creak = now + L.rand(6, 22)
		local p = L.pick(timber)
		core.sound_play("afx_creak", {pos = p, to_player = ctx.pname,
			gain = L.rand(0.3, 0.7), pitch = L.rand(0.85, 1.1), max_hear_distance = 24}, true)
	end

	-- A dungeon: a spawner, or old mossy stonework
	local spawner = find(ctx, 12, SPAWNERS)[1]
	local mossy = spawner and {} or find(ctx, 8, MOSSY)
	local anchor = spawner or (#mossy >= 15 and L.pick(mossy))
	if anchor and now >= (st.next_chain or 0) then
		st.next_chain = now + L.rand(12, 35)
		local p = vector.offset(anchor, L.rand(-4, 4), L.rand(0, 3), L.rand(-4, 4))
		core.sound_play("afx_chains", {pos = p, to_player = ctx.pname,
			gain = L.rand(0.25, 0.5), pitch = L.rand(0.8, 1.0), max_hear_distance = 20}, true)
	end
end

--[[ Other worlds ]]

local function other_worlds(ctx, wanted)
	if ctx.dimension == "nether" then
		wanted.w_nether = {sound = "afx_in_nether", gain = 0.6, fade = 0.2}
	elseif ctx.dimension == "end" then
		wanted.w_end = {sound = "afx_in_end", gain = 0.55, fade = 0.2}
		local st, now = ctx.st, ctx.now
		if now >= (st.next_chime or 0) then
			st.next_chime = now + L.rand(10, 30)
			local a = L.rand(0, 2 * math.pi)
			local p = vector.offset(ctx.ppos, math.cos(a) * 24, L.rand(4, 14), math.sin(a) * 24)
			core.sound_play("afx_end_chime", {pos = p, to_player = ctx.pname,
				gain = L.rand(0.3, 0.6), pitch = L.rand(0.8, 1.2), max_hear_distance = 64}, true)
		end
	end
end

A.register_layer(function(ctx, wanted)
	home_layer(ctx, wanted)
	cats(ctx, wanted)
	structures(ctx, wanted)
	other_worlds(ctx, wanted)
end)
