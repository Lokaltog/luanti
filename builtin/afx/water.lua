-- Water: waves lapping where the water actually meets the land, rivers
-- heard through the rock before you find them, wading and splashing.

local A = afx_ambience
local L = A.lib

local function is_water(name)
	return core.get_item_group(name, "water") > 0
end

local function solid(name)
	local def = core.registered_nodes[name]
	return def and def.walkable
end

--[[ Shorelines ]]

local SIDES = {{x = 1, y = 0, z = 0}, {x = -1, y = 0, z = 0}, {x = 0, y = 0, z = 1}, {x = 0, y = 0, z = -1}}

-- Surface water cells touching land: where waves break.
local function shore_cells(ppos, r)
	local out = {}
	-- only the surface: water with air right above it
	local waters = core.find_nodes_in_area_under_air(vector.offset(ppos, -r, -4, -r),
			vector.offset(ppos, r, 2, r), {"group:water"})
	for _, p in ipairs(waters) do
		do
			for _, d in ipairs(SIDES) do
				local side = vector.add(p, d)
				local sn = core.get_node(side).name
				-- a bank, or a beach sloping up out of the water
				if solid(sn) or (sn == "air" and solid(core.get_node(vector.offset(side, 0, -1, 0)).name)) then
					out[#out + 1] = p
					break
				end
			end
		end
	end
	return out
end

--[[ Underground rivers: heard through the rock ]]

local function underground_rivers(ctx, wanted)
	local r = 22
	local flowing = {}
	if #A.FLOWING == 0 then return end
	for _, p in ipairs(core.find_nodes_in_area(vector.offset(ctx.ppos, -r, -12, -r),
			vector.offset(ctx.ppos, r, 12, r), A.FLOWING)) do
		-- the game plays the ones close by (vl_env_sounds, env_sounds)
		if not L.game_has("water_lava") or vector.distance(p, ctx.ppos) > 9 then
			flowing[#flowing + 1] = p
		end
	end
	for _, c in ipairs(L.nearest(L.clusters(flowing), ctx.ppos, 2)) do
		if c.n >= 3 then
			wanted["ur" .. c.key] = {sound = "afx_river", pos = c.pos,
				gain = math.min(0.8, 0.2 + 0.04 * c.n), range = 40}
		end
	end
end

A.register_layer(function(ctx, wanted)
	if not L.CFG.proximity then return end
	if ctx.underground then
		underground_rivers(ctx, wanted)
		return
	end
	if ctx.dimension ~= "overworld" then return end
	local cells = shore_cells(ctx.ppos, 16)
	for _, c in ipairs(L.nearest(L.clusters(cells), ctx.ppos, 3)) do
		if c.n >= 3 then
			wanted["sh" .. c.key] = {sound = "afx_shore", pos = c.pos,
				gain = math.min(0.7, 0.15 + 0.06 * c.n), range = 28}
		end
	end
	ctx.shore = cells
end)

--[[ Swimming and diving (fast tick) ]]

A.register_tick(function(player, dtime)
	local pname = player:get_player_name()
	local st = L.player_state(pname)
	local pos = player:get_pos()
	local feet = core.get_node(vector.round(vector.offset(pos, 0, 0.2, 0))).name
	local head = core.get_node(vector.round(vector.offset(pos, 0, 1.6, 0))).name
	local in_water = is_water(feet)
	local under = is_water(head)
	local vel = player:get_velocity()
	local now = core.get_us_time() / 1e6

	-- In: a plunge, bigger the faster you hit the water
	if in_water and not st.was_in_water and vel.y < -3 then
		afx_ambience.play("afx_splash_in", {pos = pos, to_player = pname,
			gain = math.min(0.3, 0.1 + (-vel.y) / 60), pitch = L.rand(0.9, 1.05),
			max_hear_distance = 32}, true)
	end
	-- Head goes under / comes up
	if under and not st.was_under then
		st.under_since = now
	elseif not under and st.was_under then
		local held = now - (st.under_since or now)
		afx_ambience.play("afx_splash_out", {to_player = pname,
			gain = 0.18, pitch = L.rand(0.95, 1.1)}, true)
		-- a long dive, or out of breath: a gasp as you break the surface
		local breath, max_breath = player:get_breath(), player:get_properties().breath_max or 10
		if held > 6 or breath < max_breath - 2 then
			core.after(0.15, function()
				afx_ambience.play("afx_dry_gasp", {to_player = pname,
					gain = math.min(0.7, 0.3 + held / 30), pitch = L.rand(1.05, 1.15)}, true)
			end)
		end
	end
	-- Strokes: moving under water, or swimming at the surface. (Wading in
	-- the shallows is the game's own water footstep.)
	local speed = vector.length(vel)
	st.swim_d = (st.swim_d or 0) + speed * dtime
	if (under or (in_water and is_water(core.get_node(vector.round(vector.offset(pos, 0, -0.6, 0))).name)))
			and speed > 1 and st.swim_d > 1.6 then
		st.swim_d = 0
		afx_ambience.play("afx_swim", {to_player = pname,
			gain = under and 0.1 or 0.14, pitch = L.rand(0.9, 1.1)}, true)
	end
	st.was_in_water = in_water
	st.was_under = under
end)
