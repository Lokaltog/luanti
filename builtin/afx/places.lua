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
		if ent and ent.name and (ent.name:find("frame") or ent.name:find("item_entity")) then
			-- what a frame holds is drawn as an item: its name is the texture
			-- (wielditem visual) or in the mesh's
			local shows = (obj._texture or "") .. " " .. (obj._mesh or "")
			if shows:find("clock") then return obj end
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
			wanted.h_clock = {sound = "afx_clock", pos = clock, gain = 0.08, range = 6}
		else
			wanted.h_clock = {sound = "afx_clock", object = clock, gain = 0.08, range = 6}
		end
	end
end

-- A cat close by that's been sitting still a while purrs. (Whether it's
-- tamed lives on the server; the client sees it settle by you.)
local still_since = {} -- object id -> when it last moved
local function cats(ctx, wanted)
	local now = ctx.now
	for _, obj in ipairs(core.get_objects_inside_radius(ctx.ppos, 4)) do
		local name = obj._name or ""
		if name:find(":cat") or name:find("kitten") or name:find("ocelot") then
			local v = obj:get_velocity()
			if vector.length(v) > 0.2 or not still_since[obj._id] then still_since[obj._id] = now end
			if now - still_since[obj._id] > 4 then
				wanted.purr = {sound = "afx_purr", object = obj, gain = 0.5, range = 6}
				return
			end
		end
	end
	if math.random() < 0.02 then
		for id, t in pairs(still_since) do if now - t > 120 then still_since[id] = nil end end
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
		afx_ambience.play("afx_creak", {pos = p, to_player = ctx.pname,
			gain = L.rand(0.3, 0.7), pitch = L.rand(0.85, 1.1), max_hear_distance = 24}, true)
	end

	-- A dungeon: a spawner, or old mossy stonework
	local spawner = find(ctx, 12, SPAWNERS)[1]
	local mossy = spawner and {} or find(ctx, 8, MOSSY)
	local anchor = spawner or (#mossy >= 15 and L.pick(mossy))
	if anchor and now >= (st.next_chain or 0) then
		st.next_chain = now + L.rand(12, 35)
		local p = vector.offset(anchor, L.rand(-4, 4), L.rand(0, 3), L.rand(-4, 4))
		afx_ambience.play("afx_chains", {pos = p, to_player = ctx.pname,
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
			afx_ambience.play("afx_end_chime", {pos = p, to_player = ctx.pname,
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

--[[ Village bells at dawn and noon ]]

local BELLS = {}
core.register_on_mods_loaded(function()
	for name in pairs(core.registered_nodes) do
		if name:match(":bell$") or name:match(":bell_") then BELLS[#BELLS + 1] = name end
	end
end)
-- the game's own bell, when it has one (known once the server said)
local function bell_sound()
	return core.get_modpath("mcl_bells") and "mcl_bells_bell_stroke" or nil
end

local function ring(pos, pname, strokes)
	for i = 0, strokes - 1 do
		core.after(i * 2.4, function()
			if bell_sound() then
				afx_ambience.play(bell_sound(), {pos = pos, to_player = pname, gain = 0.9,
					max_hear_distance = 120}, true)
			else
				afx_ambience.play("afx_bowl", {pos = pos, to_player = pname, gain = 0.8,
					pitch = 0.5, max_hear_distance = 120}, true)
			end
		end)
	end
end

A.register_layer(function(ctx, wanted)
	if #BELLS == 0 or ctx.dimension ~= "overworld" then return end
	local st = ctx.st
	local slot = (ctx.tod > 0.245 and ctx.tod < 0.27) and "dawn" or
		(ctx.tod > 0.495 and ctx.tod < 0.52) and "noon" or nil
	if not slot then return end
	local key = slot .. (core.get_day_count and core.get_day_count() or 0)
	if st.bell_rung == key then return end
	st.bell_rung = key -- look once per slot, found or not
	local bell = core.find_nodes_in_area(vector.offset(ctx.ppos, -64, -16, -64),
		vector.offset(ctx.ppos, 64, 24, 64), BELLS)[1]
	if not bell then return end
	ring(bell, ctx.pname, slot == "dawn" and 3 or 6)
end)

--[[ Deep rock settling: a groan through the stone, gravel trickling ]]

local GRAVEL
core.register_on_mods_loaded(function()
	for _, g in ipairs({"default_gravel_footstep", "default_gravel_dug", "default_gravel_dig"}) do
		GRAVEL = GRAVEL or g -- the game's own gravel sound group
	end
end)

A.register_layer(function(ctx, wanted)
	if not ctx.underground or ctx.ppos.y > -20 or ctx.dimension ~= "overworld" then return end
	local st, now = ctx.st, ctx.now
	if not st.next_settle then st.next_settle = now + L.rand(30, 90) end
	if now < st.next_settle then return end
	st.next_settle = now + L.rand(45, 150)
	local a = L.rand(0, 2 * math.pi)
	local far = vector.offset(ctx.ppos, math.cos(a) * L.rand(16, 26), L.rand(-6, 10),
		math.sin(a) * L.rand(16, 26))
	if math.random() < 0.6 then
		afx_ambience.play("afx_rock_groan", {pos = far, to_player = ctx.pname,
			gain = L.rand(0.5, 0.9), pitch = L.rand(0.8, 1.05), max_hear_distance = 60}, true)
	else
		-- a small rockfall somewhere in the dark
		for i = 0, math.random(4, 8) do
			core.after(i * L.rand(0.08, 0.2), function()
				afx_ambience.play(GRAVEL, {pos = vector.offset(far, 0, -i * 0.3, 0),
					to_player = ctx.pname, gain = L.rand(0.3, 0.7) * (1 - i / 10),
					pitch = L.rand(0.7, 1.0), max_hear_distance = 40}, true)
			end)
		end
	end
end)

--[[ Wind chimes and lanterns answering the gusts ]]

-- Pentatonic: whatever the wind plays, it's in tune
local SCALE = {1, 9 / 8, 5 / 4, 3 / 2, 5 / 3, 2, 9 / 4}

local LANTERNS = {}
core.register_on_mods_loaded(function()
	for name in pairs(core.registered_nodes) do
		if name:find("lantern") and (name:find("ceiling") or name:find("hanging")) then
			LANTERNS[#LANTERNS + 1] = name
		end
	end
end)

local function exposed(p)
	for _, d in ipairs({{x = 1, y = 0, z = 0}, {x = -1, y = 0, z = 0}, {x = 0, y = 0, z = 1},
			{x = 0, y = 0, z = -1}, {x = 0, y = -1, z = 0}}) do
		local q = vector.add(p, d)
		local n = core.get_node_or_nil(q)
		if n and n.name == "air" and L.sky_open(q) then return true end
	end
	return false
end

A.register_layer(function(ctx, wanted)
	if ctx.dimension ~= "overworld" then return end
	local wind = ctx.wind or 0.8
	local lo, hi = vector.offset(ctx.ppos, -16, -6, -16), vector.offset(ctx.ppos, 16, 10, 16)
	-- chimes: the stronger the wind, the more notes, a little louder
	local chimes = core.find_nodes_in_area(lo, hi, {"afx_ambience:wind_chime"})
	for i = 1, math.min(#chimes, 6) do
		local p = chimes[i]
		if exposed(p) then
			-- a breeze stirs it now and then; a gust sets it ringing
			local notes = math.min(4, math.floor(math.max(0, (wind - 0.9) * 5) + math.random() * 0.55))
			for _ = 1, notes do
				core.after(L.rand(0, 1), function()
					afx_ambience.play("afx_bowl", {pos = p, to_player = ctx.pname,
						gain = L.rand(0.08, 0.15) * math.min(1.6, wind),
						pitch = 2 * SCALE[math.random(#SCALE)], max_hear_distance = 28}, true)
				end)
			end
		end
	end
	-- hanging lanterns creak on their chains in a gust
	if #LANTERNS > 0 and wind > 1.05 then
		for _, p in ipairs(core.find_nodes_in_area(lo, hi, LANTERNS)) do
			if exposed(p) and math.random() < (wind - 1) * 0.6 then
				afx_ambience.play("afx_creak", {pos = p, to_player = ctx.pname,
					gain = L.rand(0.08, 0.16), pitch = L.rand(1.6, 2.0), max_hear_distance = 16}, true)
			end
		end
	end
end)

--[[ The build settles: a wooden house creaks now and then, more as it
cools in the evening and when the wind leans on it. (The engine gives every
room its own faint tone; this is the timber talking.) ]]

A.register_layer(function(ctx, wanted)
	if not ctx.indoors or ctx.dimension ~= "overworld" then return end
	local st = ctx.st
	if ctx.now < (st.next_timber or ctx.now) then return end
	if not st.next_timber then
		st.next_timber = ctx.now + L.rand(15, 40)
		return
	end
	local wood = core.find_nodes_in_area(vector.offset(ctx.ear, -5, -2, -5),
			vector.offset(ctx.ear, 5, 5, 5), {"group:wood", "group:tree"})
	if #wood < 30 then
		st.next_timber = ctx.now + 10
		return
	end
	-- evening cooling and gusts make it talk more
	local cooling = (ctx.tod > 0.74 and ctx.tod < 0.9) and 2 or 1
	local wind = math.max(1, (ctx.wind or 0.8) * 1.2)
	st.next_timber = ctx.now + L.rand(30, 100) / (cooling * wind)
	-- somewhere above or in the walls, not under your feet
	local p
	for _ = 1, 6 do
		local c = L.pick(wood)
		if c.y >= ctx.ear.y - 1 then p = c break end
	end
	if not p then return end
	local deep = math.random() < 0.6
	afx_ambience.play("afx_creak", {pos = p, to_player = ctx.pname,
		gain = deep and L.rand(0.05, 0.09) or L.rand(0.03, 0.05),
		pitch = deep and L.rand(0.55, 0.8) or L.rand(1.3, 1.7), max_hear_distance = 12}, true)
	-- the settle sometimes answers itself a moment later, elsewhere
	if math.random() < 0.3 then
		local q = L.pick(wood)
		core.after(L.rand(0.6, 2.2), function()
			afx_ambience.play("afx_creak", {pos = q, to_player = ctx.pname,
				gain = L.rand(0.02, 0.04), pitch = L.rand(0.9, 1.4), max_hear_distance = 10}, true)
		end)
	end
end)

-- what /afx_report shows from here
core.register_on_mods_loaded(function()
	if not afx_ambience.add_report then return end
	afx_ambience.add_report(function()
		local function list(t) return #t .. " " .. table.concat(t, " ", 1, math.min(#t, 6)) end
		return {
			"home nodes: " .. list(HOME), "rails: " .. list(RAILS), "timber: " .. list(TIMBER),
			"spawners: " .. list(SPAWNERS), "mossy: " .. list(MOSSY), "bells: " .. list(BELLS),
			"bell sound: " .. tostring(bell_sound()), "lanterns: " .. list(LANTERNS),
		}
	end)
end)
