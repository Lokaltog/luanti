-- Weather: gusting wind, rain that builds and drips after, thunder that
-- rolls. The engine delays distant one-shots by distance / 343 s, so a
-- strike's thunder arrives after the flash on its own.

local A = afx_ambience
local L = A.lib

--[[ Gusts ]]

-- Wind strength over minutes: two slow swells plus discrete gusts that
-- rise over a few seconds, hold and die away. Shared by everyone (it's the
-- weather), shaped per player by exposure.
local gust = {level = 0, phase = "idle", t = 0, next = 20, peak = 1}

local function step_gust(dtime)
	gust.t = gust.t + dtime
	if gust.phase == "idle" then
		gust.next = gust.next - dtime
		if gust.next <= 0 then
			gust.phase, gust.t, gust.peak = "rise", 0, L.rand(0.6, 1.4)
		end
	elseif gust.phase == "rise" then
		gust.level = gust.peak * math.min(1, gust.t / 3)
		if gust.t >= 3 then gust.phase, gust.t = "hold", 0 end
	elseif gust.phase == "hold" then
		if gust.t >= L.rand(2, 6) then gust.phase, gust.t = "fall", 0 end
	elseif gust.phase == "fall" then
		gust.level = gust.peak * math.max(0, 1 - gust.t / 7)
		if gust.t >= 7 then
			gust.phase, gust.level, gust.next = "idle", 0, L.rand(15, 60)
		end
	end
end

function A.wind_now()
	local t = core.get_gametime() or 0
	local swell = 0.75 + 0.15 * math.sin(t * 2 * math.pi / 180) + 0.1 * math.sin(t * 2 * math.pi / 47 + 1)
	return swell + 0.6 * gust.level
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

local rain = {on = false, since = 0, stopped = -1000}

local function rain_layer(ctx, wanted)
	local now = ctx.now
	local raining = ctx.rain > 0
	if raining and not rain.on then
		rain.on, rain.since = true, now
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

	local st = ctx.st
	-- The first fat drops: single splats on surfaces near you
	if raining and now - rain.since < 12 then
		if not st.next_drop or now >= st.next_drop then
			st.next_drop = now + L.rand(0.3, 1.2)
			local p = vector.offset(ctx.ppos, L.rand(-5, 5), L.rand(0, 3), L.rand(-5, 5))
			core.sound_play("afx_drip", {pos = p, to_player = ctx.pname,
				gain = L.rand(0.15, 0.4), pitch = L.rand(0.6, 0.9), max_hear_distance = 16}, true)
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
	-- for a little after
	local gutter = raining and 1 or math.max(0, 1 - after / 40)
	if gutter > 0 and not ctx.under_sky and not ctx.underground then
		local ear = ctx.ear
		for dx = -5, 5 do
			for dz = -5, 5 do
				if math.abs(dx) == 5 or math.abs(dz) == 5 then
					-- an eave: air at ear height out here, open sky above it
					local p = vector.offset(ear, dx, 0, dz)
					local n = core.get_node_or_nil(p)
					if n and n.name == "air" and L.sky_open(p) then
						-- the roof edge just inward of it, 1-5 nodes up
						local ix = dx - (dx > 0 and 1 or dx < 0 and -1 or 0)
						local iz = dz - (dz > 0 and 1 or dz < 0 and -1 or 0)
						local roofed = false
						for up = 1, 5 do
							local rn = core.get_node_or_nil(vector.offset(ear, ix, up, iz))
							local rdef = rn and core.registered_nodes[rn.name]
							if rdef and rdef.walkable then roofed = true break end
						end
						if roofed and not wanted.gutter then
							wanted.gutter = {sound = "afx_gutter", pos = p,
								gain = 0.35 * gutter * (ctx.rain_ramp or 1), range = 14}
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
	rain_layer(ctx, wanted)
end)

core.register_globalstep(step_gust)

--[[ Thunder: a crack up close, a long roll far away ]]

-- `game_crack`: the game plays its own thunder at the strike (VoxeLibre's
-- lightning mod); then only the far roll is ours.
local function on_strike(pos, game_crack)
	if not pos then return end
	for _, player in ipairs(core.get_connected_players()) do
		local d = vector.distance(player:get_pos(), pos)
		local pname = player:get_player_name()
		if d < 160 then
			if not game_crack then
				core.sound_play("afx_thunder_near", {pos = pos, to_player = pname,
					gain = 1.0, max_hear_distance = 1000}, true)
			end
		else
			-- Far strikes: just the roll, quieter with distance
			core.sound_play("afx_thunder_far", {pos = pos, to_player = pname,
				gain = math.max(0.3, 1.2 - d / 800), max_hear_distance = 1200}, true)
		end
	end
end

core.register_on_mods_loaded(function()
	local lightning = rawget(_G, "lightning")
	if lightning and lightning.register_on_strike then
		lightning.register_on_strike(function(pos, pos2) on_strike(pos2 or pos, true) end)
	end
end)

A.thunder_at = on_strike

-- For tests and scripted moments: start a gust now
function A.gust_now(peak)
	gust.phase, gust.t, gust.peak = "rise", 0, peak or 1.2
end

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
	if ctx.under_sky or ctx.underground or ctx.dimension ~= "overworld" then return end
	local wind = ctx.wind or 0
	if wind < 0.85 then return end
	local airs = core.find_nodes_in_area(vector.offset(ctx.ear, -6, -2, -6),
			vector.offset(ctx.ear, 6, 3, 6), {"air"})
	local gaps = {}
	for _, p in ipairs(airs) do
		local w = is_gap(p)
		if w then gaps[#gaps + 1] = {pos = p, narrow = w == 4} end
	end
	table.sort(gaps, function(a, b)
		return vector.distance(a.pos, ctx.ear) < vector.distance(b.pos, ctx.ear)
	end)
	for i = 1, math.min(2, #gaps) do
		local g = gaps[i]
		-- a pinhole whistles higher and thinner than a window
		wanted["draft" .. i] = {sound = "afx_draft", pos = g.pos,
			gain = math.min(0.7, (wind - 0.85) * 1.2) * (g.narrow and 0.8 or 0.6),
			fade = 0.4, range = 14, pitch = g.narrow and 1.3 or 1.0}
	end
end)

-- How wet the ground is: 1 while it rains, drying over two minutes after
function A.wetness(now)
	if rain.on then return math.min(1, (now - rain.since) / 20) end
	return math.max(0, 1 - (now - rain.stopped) / 120)
end
