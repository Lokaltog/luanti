-- review repro: tall grass walk + jungle-plank house in rain (VoxeLibre)
local name
local function P() return core.get_player_by_name(name) end
local function say(msg) core.log("action", "afxplay: " .. msg) end
local function v(x, y, z) return {x = x, y = y, z = z} end

local function biome_at(x, z)
	local bd = core.get_biome_data(v(x, 20, z))
	return bd and core.get_biome_name(bd.biome) or ""
end
local function find_biome(pred)
	for r = 0, 3000, 96 do
		for a = 0, 15 do
			local x = math.floor(r * math.cos(a * math.pi / 8))
			local z = math.floor(r * math.sin(a * math.pi / 8))
			local b = biome_at(x, z)
			if pred(b) then return v(x, 0, z), b end
		end
	end
end
local function ground(p)
	for y = 120, -10, -1 do
		local n = core.get_node(v(p.x, y, p.z)).name
		local def = core.registered_nodes[n]
		if def and def.walkable and core.get_item_group(n, "leaves") == 0 and
				core.get_item_group(n, "tree") == 0 then
			return v(p.x, y + 1, p.z)
		end
	end
end
local function emerge(center, r, cb)
	core.emerge_area(vector.offset(center, -r, -70, -r), vector.offset(center, r, 90, r),
		function(_, _, remaining) if remaining == 0 then cb() end end)
end
local function weather(w)
	mcl_weather.change_weather(w, core.get_gametime() + 3600, "afxplay")
end
local function go(pos, look)
	local pl = P()
	pl:set_pos(pos)
	pl:set_look_horizontal(look or 0)
	pl:set_look_vertical(0)
end
local function where_log()
	local pl = P()
	local ear = vector.round(vector.offset(pl:get_pos(), 0, 1.6, 0))
	local ctx = {player = pl, daylight = core.get_natural_light(ear, 0.5) or 0,
		under_sky = afx_ambience.lib.sky_open(ear)}
	afx_ambience.where(ctx)
	local h = ctx.hearing
	say(("where %s: hearing=%s sky=%s exposure=%.2f through=%.3f underground=%s indoors=%s encl=%s light=%s rain=%s"):format(
		core.pos_to_string(pl:get_pos()), tostring(h ~= nil), tostring(ctx.under_sky), ctx.exposure, ctx.through,
		tostring(ctx.underground), tostring(ctx.indoors), h and ("%.2f"):format(h.enclosure) or "-",
		tostring(core.get_node_light(ear, 0.5)), tostring(afx_ambience.rain_at(pl:get_pos()))))
end

local walking
local function walk(from, speed, secs)
	local t = 0
	walking = true
	local function step()
		if not walking then return end
		t = t + 0.05
		P():set_pos(vector.offset(from, speed * t, 0, 0))
		if t < secs then core.after(0.05, step) else walking = false end
	end
	step()
end

local function strip(c, len, plant)
	for x = -2, len + 2 do
		for z = -2, 2 do
			core.set_node(vector.offset(c, x, -1, z), {name = "mcl_core:dirt_with_grass"})
			for y = 0, 3 do core.set_node(vector.offset(c, x, y, z), {name = "air"}) end
			if plant == "tall" then
				core.set_node(vector.offset(c, x, 0, z), {name = "mcl_flowers:tallgrass"})
			elseif plant == "double" then
				core.set_node(vector.offset(c, x, 0, z), {name = "mcl_flowers:double_grass"})
				core.set_node(vector.offset(c, x, 1, z), {name = "mcl_flowers:double_grass_top"})
			end
		end
	end
end

local function house(c, leafroof)
	for x = -4, 4 do for y = -1, 6 do for z = -4, 4 do
		local wall = math.abs(x) == 4 or math.abs(z) == 4
		local nm = "air"
		if wall or y == -1 or y == 6 then
			nm = "mcl_core:junglewood"
			if math.abs(x) == 4 and math.abs(z) == 4 then nm = "mcl_core:jungletree" end
		end
		if x == 4 and z == 0 and (y == 1 or y == 2) then nm = "xpanes:pane_natural_flat" end
		core.set_node(vector.offset(c, x, y, z), {name = nm})
	end end end
	for x = -6, 6 do for z = -6, 6 do for y = 7, 14 do
		core.set_node(vector.offset(c, x, y, z), {name = "air"})
	end end end
	if leafroof then
		for x = -5, 5 do for z = -5, 5 do for y = 7, 8 do
			core.set_node(vector.offset(c, x, y, z), {name = "mcl_core:jungleleaves"})
		end end end
	end
end

local SP = {}
local function run(steps, i)
	i = i or 1
	local s = steps[i]
	if not s then
		say("done")
		core.request_shutdown("afxplay done", false, 1)
		return
	end
	say("step " .. s.log)
	s.fn()
	core.after(2, where_log)
	core.after(s.wait, run, steps, i + 1)
end

local function tour()
	say("start")
	local g = SP.plains
	local gr1, gr2, gr3 = vector.offset(g, 0, 0, 0), vector.offset(g, 0, 0, 8), vector.offset(g, 0, 0, 16)
	strip(gr1, 40, nil); strip(gr2, 40, "tall"); strip(gr3, 40, "double")
	-- the house goes where it actually rains (the biome next door may be dry)
	local hc = vector.offset(g, 0, 0, 30)
	local w = rawget(_G, "mcl_weather")
	if w and w.has_rain then
		for r = 30, 90, 10 do
			local found = false
			for a = 0, 7 do
				local q = vector.offset(g, math.floor(r * math.cos(a * math.pi / 4)), 0,
					math.floor(r * math.sin(a * math.pi / 4)))
				local ok, rainy = pcall(w.has_rain, q)
				local ok2, snowy = pcall(w.has_snow or function() return false end, q)
				if ok and rainy and not (ok2 and snowy) then hc, found = ground(q) or q, true break end
			end
			if found then break end
		end
	end
	local jc = SP.jungle
	run({
		{log = "grass control (no plants), walk 4.3/s", wait = 9, fn = function()
			weather("none"); core.set_timeofday(0.5); walk(gr1, 4.3, 8) end},
		{log = "tallgrass walk 4.3/s", wait = 9, fn = function() walk(gr2, 4.3, 8) end},
		{log = "double grass walk 4.3/s", wait = 9, fn = function() walk(gr3, 4.3, 8) end},
		{log = "tallgrass sprint 5.6/s", wait = 7, fn = function() walk(gr2, 5.6, 6) end},
		{log = "plains house dry", wait = 8, fn = function() house(hc, false); go(vector.offset(hc, -1, 0, -1)) end},
		{log = "plains house rain", wait = 36, fn = function() weather("rain") end},
		{log = "plains house rain, leaf roof", wait = 16, fn = function() house(hc, true); go(vector.offset(hc, -1, 0, -1)) end},
		{log = "jungle house rain", wait = 20, fn = function()
			house(jc, false); go(vector.offset(jc, -1, 0, -1)) end},
		{log = "jungle outdoors rain", wait = 12, fn = function() go(vector.offset(jc, 8, 0, 8)) end},
		{log = "rain stops, jungle house", wait = 20, fn = function()
			weather("none"); go(vector.offset(jc, -1, 0, -1)) end},
		{log = "end", wait = 1, fn = function() end},
	})
end

core.register_on_joinplayer(function(player)
	name = player:get_player_name()
	core.settings:set("time_speed", "0")
	player:set_physics_override({gravity = 0, speed = 0, jump = 0})
	weather("none")
	local p, pb = find_biome(function(b) return b == "Plains" or b == "SunflowerPlains" end)
	local j, jb = find_biome(function(b) return b == "Jungle" or b == "JungleM" end)
	say(("found %s %s, %s %s"):format(pb, core.pos_to_string(p), tostring(jb), core.pos_to_string(j or v(0,0,0))))
	local todo = 2
	local function done()
		todo = todo - 1
		if todo > 0 then return end
		SP.plains = ground(p)
		SP.jungle = ground(j or vector.offset(p, 80, 0, 0))
		say("spots plains " .. core.pos_to_string(SP.plains) .. " jungle " .. core.pos_to_string(SP.jungle))
		core.after(1, tour)
	end
	emerge(p, 48, done)
	emerge(j or vector.offset(p, 80, 0, 0), 24, done)
end)
