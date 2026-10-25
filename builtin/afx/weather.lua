-- Weather: gusting wind, rain that builds and drips after, thunder that
-- rolls. The engine delays distant one-shots by distance / 343 s, so a
-- strike's thunder arrives after the flash on its own.

local A = afx_ambience
local L = A.lib

--[[ Wind: the server's weather (afx_ambience's bridge) ]]

-- Wind strength over minutes (slow swells and gusts), before this spot's
-- exposure shapes it
function A.wind_now()
	return tonumber(core.afx.world.wind_raw) or 0.8
end

-- Where the wind blows (a unit vector toward where it blows)
function A.wind_dir()
	local w = core.afx.world
	local x, z = tonumber(w.wind_dir_x) or 1, tonumber(w.wind_dir_z) or 0
	local l = math.sqrt(x * x + z * z)
	if l < 1e-3 then return {x = 1, y = 0, z = 0} end
	return {x = x / l, y = 0, z = z / l}
end

-- How exposed the ear is: height above the ground around (ridges, peaks).
local DIRS = {}
for i = 0, 7 do
	DIRS[#DIRS + 1] = {x = math.cos(i * math.pi / 4) * 10, z = math.sin(i * math.pi / 4) * 10}
end

local function exposure(ppos)
	local sum, n = 0, 0
	for _, d in ipairs(DIRS) do
		local x, z = math.floor(ppos.x + d.x + 0.5), math.floor(ppos.z + d.z + 0.5)
		for dy = 4, -24, -1 do
			local y = math.floor(ppos.y) + dy
			local node = core.get_node_or_nil({x = x, y = y, z = z})
			if not node then break end
			local def = core.registered_nodes[node.name]
			if def and def.walkable and core.get_item_group(node.name, "leaves") == 0 then
				sum, n = sum + (ppos.y - y - 1), n + 1
				break
			end
		end
	end
	if n == 0 then return 1 end -- nothing below at all: a cliff edge or the sky
	return math.max(0, math.min(1, (sum / n) / 12))
end

--[[ Rain: first drops, then the downpour; dripping and gutters after ]]

-- Per player: one in a downpour, another in a dry desert
local function rain_of(st)
	st.rain = st.rain or {on = false, since = 0, stopped = -1000, last_gutter = 0}
	return st.rain
end

local function solid_top(p)
	local n = core.get_node_or_nil(p)
	local d = n and core.registered_nodes[n.name]
	return d and d.walkable
end

local function rain_layer(ctx, wanted)
	local now = ctx.now
	local raining = ctx.rain > 0
	local rain = rain_of(ctx.st)
	if raining and not rain.on then
		-- back in it after a moment out (a biome edge): the storm carries on
		if now - rain.stopped > 30 then rain.since = now end
		rain.on = true
	elseif not raining and rain.on then
		rain.on, rain.stopped = false, now
	end
	-- Birds come back a while after the rain, one by one
	ctx.birds_back = raining and 0 or math.max(0, math.min(1, (now - rain.stopped - 12) / 30))
	if not raining and now - rain.stopped < 12 then
		ctx.st.next_bird = math.max(ctx.st.next_bird or 0, rain.stopped + L.rand(14, 30))
	end
	-- Surface layers swell in over the first half minute
	ctx.rain_ramp = raining and math.min(1, 0.15 + (now - rain.since) / 30) or 1

	-- The front: rain marches in from upwind over ~25 s (the engine fades and
	-- steers the game's rain bed by it), and after it stops our own tail
	-- recedes downwind
	local wd = A.wind_dir()
	if raining then
		ctx.rain_front = math.min(1, (now - rain.since) / 25)
		ctx.rain_front_dir = {x = -wd.x, z = -wd.z}
	else
		ctx.rain_front = math.max(0, 1 - (now - rain.stopped) / 22)
		ctx.rain_front_dir = {x = wd.x, z = wd.z}
		if ctx.rain_front > 0 and (rain.was_heavy or 0) > 0 and ctx.dimension == "overworld" then
			wanted.rain_tail = {sound = "afx_out_rain", gain = 0.55 * rain.was_heavy, fade = 0.5}
		end
	end
	if raining then rain.was_heavy = math.max(rain.was_heavy or 0, math.min(1, ctx.rain)) end
	if not raining and ctx.rain_front <= 0 then rain.was_heavy = 0 end

	-- Rain on fire and lava out in it: a sizzle
	if raining and ctx.dimension == "overworld" then
		local hot = core.find_nodes_in_area(vector.offset(ctx.ppos, -10, -4, -10),
				vector.offset(ctx.ppos, 10, 6, 10), {"group:lava", "group:fire"})
		local exposed = {}
		for _, p in ipairs(hot) do
			if L.sky_open(vector.offset(p, 0, 1, 0)) then exposed[#exposed + 1] = p end
		end
		for _, c in ipairs(L.nearest(L.clusters(exposed), ctx.ppos, 2)) do
			wanted["sz" .. c.key] = {sound = "afx_sizzle", pos = c.pos,
				gain = math.min(0.45, 0.1 + 0.04 * c.n) * ctx.rain * (ctx.rain_ramp or 1), range = 18}
		end
	end

	local st = ctx.st
	-- The first fat drops: single splats on surfaces near you that the rain
	-- actually reaches (out here, not in your room or inside the walls)
	if raining and now - rain.since < 12 and not ctx.indoors and not ctx.underground and
			(ctx.under_sky or ctx.exposure > 0.4) then
		if not st.next_drop or now >= st.next_drop then
			st.next_drop = now + L.rand(0.3, 1.2)
			for _ = 1, 4 do
				local c = vector.round(vector.offset(ctx.ppos, L.rand(-5, 5), 3, L.rand(-5, 5)))
				-- down to the first solid top under open sky
				local played = false
				for dy = 0, -6, -1 do
					local q = vector.offset(c, 0, dy, 0)
					if solid_top(q) then
						if L.sky_open(vector.offset(q, 0, 1, 0)) then
							-- a real drop hitting it: soft on leaves and ground, a
							-- tap on planks, stone and roofs
							local nn = core.get_node(q).name
							local soft = core.get_item_group(nn, "leaves") > 0 or
									core.get_item_group(nn, "soil") > 0 or
									core.get_item_group(nn, "dirt") > 0 or nn:find("grass") ~= nil
							afx_ambience.play(soft and "afx_raindrop" or "afx_raindrop_hard",
								{pos = vector.offset(q, 0, 0.5, 0), to_player = ctx.pname,
								gain = L.rand(0.2, 0.45), pitch = L.rand(0.92, 1.12),
								max_hear_distance = 16}, true)
							played = true
						end
						break
					end
				end
				if played then break end
			end
		end
	end

	-- After the rain: the canopy and the eaves keep dripping for a while
	local after = now - rain.stopped
	local tail = raining and 0 or math.max(0, 1 - after / 90)
	if tail > 0 and ctx.dimension == "overworld" then
		local leaves = core.find_nodes_in_area(vector.offset(ctx.ppos, -8, 0, -8),
				vector.offset(ctx.ppos, 8, 10, 8), {"group:leaves"})
		for _, c in ipairs(L.nearest(L.clusters(leaves), ctx.ppos, 3)) do
			if c.n >= 6 then
				wanted["ad" .. c.key] = {sound = "afx_drip_after", pos = c.pos,
					gain = 0.5 * tail * math.min(1, c.n / 20), range = 16, fade = 0.05}
			end
		end
	end

	-- Gutters: under a roof, water running off its edge while it rains and
	-- for a little after (dying away from where it was, never swelling)
	local gutter = raining and (ctx.rain_ramp or 1) or
			(rain.last_gutter or 0) * math.max(0, 1 - after / 40)
	if raining then rain.last_gutter = gutter end
	-- only where the outside is in earshot through air: a porch, a doorway
	local got_gutter = false
	-- first where the engine says the way out is: water running off the
	-- roof edge right over the doorway you'd step out of
	local op = gutter > 0 and not ctx.under_sky and not ctx.underground and L.opening(ctx)
	if op and op.size <= 12 then
		local roofed = false
		for up = 1, 3 do
			-- an overhang outside the door, not the wall above it
			if solid_top(vector.offset(op.exit, 0, up, 0)) then roofed = true break end
		end
		local n = core.get_node_or_nil(op.exit)
		if roofed and n and n.name == "air" then
			got_gutter = true
			wanted["gutter" .. core.hash_node_position(vector.round(op.exit))] = {sound = "afx_gutter",
				pos = op.exit, gain = 0.35 * gutter, range = 14}
		elseif raining and n and n.name == "air" then
			-- no eave: the rain itself, coming down just outside the door, on
			-- whatever is out there
			-- the ground under the doorstep
			local bn, ground = "", vector.round(op.exit)
			for _ = 1, 4 do
				ground = vector.offset(ground, 0, -1, 0)
				if solid_top(ground) then bn = core.get_node(ground).name break end
			end
			local snd, g = L.rain_sound_for(bn)
			if bn == "" then snd = nil end
			-- on the ground (grass, dirt, sand) it patters rather than drums
			if core.get_item_group(bn, "soil") > 0 or core.get_item_group(bn, "dirt") > 0 or
					core.get_item_group(bn, "sand") > 0 or bn:find("grass") then
				snd, g = "afx_rain_leaves", 0.35
			end
			if snd then
				wanted.door_rain = {sound = snd, pos = vector.offset(ground, 0, 0.6, 0),
					gain = 0.25 * g * ctx.rain * (ctx.rain_ramp or 1), range = 16}
			end
		end
	end
	if not got_gutter and gutter > 0 and not ctx.under_sky and not ctx.underground and
			ctx.exposure > 0.05 then
		local ear = ctx.ear
		for dx = -5, 5 do
			for dz = -5, 5 do
				if math.abs(dx) == 5 or math.abs(dz) == 5 then
					-- an eave: air at ear height out here, open sky above it
					local p = vector.offset(ear, dx, 0, dz)
					local n = core.get_node_or_nil(p)
					if n and n.name == "air" and L.sky_open(p) then
						-- a real overhang: air just inward of it at ear height (not
						-- a wall), with the roof edge 1-3 nodes above that
						local ix = dx - (dx > 0 and 1 or dx < 0 and -1 or 0)
						local iz = dz - (dz > 0 and 1 or dz < 0 and -1 or 0)
						local inward = core.get_node_or_nil(vector.offset(ear, ix, 0, iz))
						local roofed = false
						if inward and inward.name == "air" then
							for up = 1, 3 do
								if solid_top(vector.offset(ear, ix, up, iz)) then roofed = true break end
							end
						end
						if roofed and not got_gutter then
							local key = "gutter" .. core.hash_node_position(p)
							got_gutter = true
							wanted[key] = {sound = "afx_gutter", pos = p, gain = 0.35 * gutter, range = 14}
						end
					end
				end
			end
		end
	end
end

A.register_layer(function(ctx, wanted)
	if ctx.dimension ~= "overworld" then
		ctx.wind = 0.6
		return
	end
	ctx.wind = A.wind_now() * (0.7 + 0.8 * exposure(ctx.ppos))
	ctx.st.wind = ctx.wind
	rain_layer(ctx, wanted)
	-- tell the engine, so distant sounds travel with the weather (a test
	-- scenario on the server can fix the wind)
	local d = A.wind_dir()
	local w = tonumber(core.afx.world.wind_fixed) or ctx.wind
	local fd = ctx.rain_front_dir or {x = 0, z = 0}
	core.afx_set_world({wind_x = d.x, wind_z = d.z, wind = w,
		rain = math.min(1, ctx.rain + 0.5 * ctx.snow),
		rain_front = ctx.rain_front or 1, front_x = fd.x, front_z = fd.z})
end)

--[[ Thunder: a crack up close, a long roll far away ]]

-- `game_crack`: the game plays its own thunder at the strike (VoxeLibre's
-- lightning mod); then only the far roll is ours.
-- How much of the sky the player sees: out in the open all of it, from a
-- doorway or a window a little (the engine's open path to the outdoors)
local function sky_seen()
	local h = core.afx_hearing()
	if not h then return 0.5 end
	return math.max(h.sky or 0, math.min(1, (h.outdoor or 0) * 1.5), 0.03)
end

local function on_strike(pos, game_crack)
	if not pos then return end
	if A.rattle then A.rattle(pos, 1.3) end
	-- the flash, now; the thunder after it, as late as the distance (the
	-- engine delays it): picture and sound from the same moment
	local p0 = core.get_connected_players()[1]
	local d0 = p0 and vector.distance(p0:get_pos(), pos) or 100
	-- (by day it's a pale blink; at night it lights the world)
	local tod = core.get_timeofday()
	local day = math.max(0, math.min(1, (0.32 - math.abs(tod - 0.5)) / 0.05))
	core.afx_flash("lightning", pos, math.max(0.25, math.min(1, 1.2 - d0 / 600)),
		sky_seen() * (0.3 + 0.7 * (1 - day)))
	for _, player in ipairs(core.get_connected_players()) do
		local d = vector.distance(player:get_pos(), pos)
		local pname = player:get_player_name()
		if d < 160 then
			if not game_crack then
				afx_ambience.play("afx_thunder_near", {pos = pos, to_player = pname,
					gain = 1.0, max_hear_distance = 1000}, true)
			end
		elseif game_crack and d < 500 then
			-- the game's own thunder already reaches this far
		else
			-- Far strikes: just the roll, quieter with distance
			afx_ambience.play("afx_thunder_far", {pos = pos, to_player = pname,
				gain = math.max(0.3, 1.2 - d / 800), max_hear_distance = 1200}, true)
		end
	end
end

A.thunder_at = on_strike

-- from the server: "x y z crack" (crack 1: the game plays its own at the strike)
core.afx.register_on_data("strike", function(data)
	local x, y, z, crack = data:match("^(%S+) (%S+) (%S+) ?(%S*)")
	if not z then return end
	on_strike({x = tonumber(x), y = tonumber(y), z = tonumber(z)}, crack == "1")
end)

--[[ Windows rattle when a big sound arrives: thunder, a blast ]]

local rattle_until = {}
local function rattle(pos, loudness)
	local now = core.get_us_time() / 1e6
	for _, player in ipairs(core.get_connected_players()) do
		local ppos = player:get_pos()
		local d = vector.distance(ppos, pos)
		local k = loudness * math.min(1, 60 / math.max(1, d))
		local pn = player:get_player_name()
		-- a string of blasts rattles the panes once, not once per blast
		if k > 0.12 and now >= (rattle_until[pn] or 0) then
			rattle_until[pn] = now + 0.6
			local panes = core.find_nodes_in_area(vector.offset(ppos, -8, -3, -8),
					vector.offset(ppos, 8, 6, 8), {"group:pane", "group:glass"})
			local pname = player:get_player_name()
			local arrive = d / 343 + 0.05
			for i = 1, math.min(2, #panes) do
				local p = panes[math.random(#panes)]
				local taps = math.random(4, 6)
				for t = 0, taps - 1 do
					core.after(arrive + t * L.rand(0.035, 0.06) + i * 0.02, function()
						A.play("afx_raindrop_hard", {pos = p, to_player = pname,
							gain = 0.22 * k * (1 - t / taps), pitch = L.rand(1.7, 2.3),
							max_hear_distance = 14}, true)
					end)
				end
			end
		end
	end
end
A.rattle = rattle

-- explosions, wherever the game sets them off: "x y z loudness"
core.afx.register_on_data("blast", function(data)
	local x, y, z, k = data:match("^(%S+) (%S+) (%S+) (%S+)")
	if not k then return end
	local pos = {x = tonumber(x), y = tonumber(y), z = tonumber(z)}
	rattle(pos, tonumber(k) or 1)
	local p0 = core.get_connected_players()[1]
	local d0 = p0 and vector.distance(p0:get_pos(), pos) or 30
	local near = math.min(1, 24 / math.max(1, d0)) * math.min(1.5, tonumber(k) or 1)
	if near > 0.05 then core.afx_flash("blast", pos, near, 0.25 * near * sky_seen()) end
end)

-- a gust rising (the server's weather): "peak"
core.afx.register_on_data("gust", function(data)
	if A.on_gust then A.on_gust(tonumber(data) or 1) end
end)

--[[ Drafts: gaps in walls whistle when the wind is up ]]

local AXES = {{x = 1, y = 0, z = 0}, {x = 0, y = 0, z = 1}, {x = 0, y = 1, z = 0}}

local function walkable(p)
	local n = core.get_node_or_nil(p)
	local d = n and core.registered_nodes[n.name]
	return d and d.walkable
end

-- An air cell in a wall: open sky on one side, a sheltered inside on the
-- other, and wall around it (at most two of its four sides open).
local function is_gap(p)
	for _, a in ipairs(AXES) do
		local front, back = vector.add(p, a), vector.subtract(p, a)
		local fn, bn = core.get_node_or_nil(front), core.get_node_or_nil(back)
		if fn and bn and fn.name == "air" and bn.name == "air" then
			local fo, bo = L.sky_open(front), L.sky_open(back)
			if fo ~= bo then
				local walls = 0
				for _, b in ipairs(AXES) do
					if b ~= a then
						if walkable(vector.add(p, b)) then walls = walls + 1 end
						if walkable(vector.subtract(p, b)) then walls = walls + 1 end
					end
				end
				if walls >= 3 then return walls end
			end
		end
	end
end

A.register_layer(function(ctx, wanted)
	if not ctx.indoors or ctx.dimension ~= "overworld" then return end
	local wind = ctx.wind or 0
	if wind < 0.85 then return end
	-- Gaps are looked for every 10 s or after moving 3 nodes, not every second
	local st = ctx.st
	local gaps = st.gaps
	if not gaps or ctx.now > (st.gaps_t or 0) + 10 or
			vector.distance(st.gaps_at or ctx.ear, ctx.ear) > 3 then
		gaps = {}
		local airs = core.find_nodes_in_area(vector.offset(ctx.ear, -6, -2, -6),
				vector.offset(ctx.ear, 6, 3, 6), {"air"})
		-- the ones in walls: next to something solid; then at most 160 of them
		local cand = {}
		for _, p in ipairs(airs) do
			if walkable(vector.offset(p, 1, 0, 0)) or walkable(vector.offset(p, -1, 0, 0)) or
					walkable(vector.offset(p, 0, 0, 1)) or walkable(vector.offset(p, 0, 0, -1)) then
				cand[#cand + 1] = p
			end
		end
		for i = 1, math.min(#cand, 160) do
			local p = #cand > 160 and cand[math.random(#cand)] or cand[i]
			local w = is_gap(p)
			if w then gaps[#gaps + 1] = {pos = p, narrow = w == 4} end
		end
		st.gaps, st.gaps_t, st.gaps_at = gaps, ctx.now, ctx.ear
	end
	table.sort(gaps, function(a, b)
		return vector.distance(a.pos, ctx.ear) < vector.distance(b.pos, ctx.ear)
	end)
	-- the gap the engine hears the outdoors through comes first: a crack
	-- or a narrow slot on the way out
	local op = L.opening(ctx)
	if op and op.size <= 3 then
		local inner = vector.round(op.inner)
		local seen = false
		for _, g in ipairs(gaps) do
			if vector.equals(g.pos, inner) then seen = true end
		end
		if not seen then table.insert(gaps, 1, {pos = inner, narrow = op.size <= 1}) end
	end
	for i = 1, math.min(2, #gaps) do
		local g = gaps[i]
		-- a pinhole whistles higher and thinner than a window
		wanted["draft" .. core.hash_node_position(g.pos)] = {sound = "afx_draft", pos = g.pos,
			gain = math.min(0.7, (wind - 0.85) * 1.2) * (g.narrow and 0.8 or 0.6),
			fade = 0.4, range = 14, pitch = g.narrow and 1.3 or 1.0}
	end
end)

-- How wet the ground is where this player is: 1 while it rains, drying
-- over two minutes after
function A.wetness(now, pname)
	local rain = rain_of(L.player_state(pname))
	if rain.on then return math.min(1, (now - rain.since) / 20) end
	return math.max(0, 1 - (now - rain.stopped) / 120)
end
