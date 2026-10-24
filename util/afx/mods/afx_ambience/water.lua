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
	for _, p in ipairs(core.find_nodes_in_area(vector.offset(ctx.ppos, -r, -12, -r),
			vector.offset(ctx.ppos, r, 12, r), {"group:water"})) do
		if core.get_node(p).name:find("flowing") then
			-- the game plays the ones close by (vl_env_sounds, env_sounds)
			if not L.GAME_HAS_WATER_LAVA or vector.distance(p, ctx.ppos) > 9 then
				flowing[#flowing + 1] = p
			end
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

--[[ Wading and splashing (fast tick) ]]

A.register_tick(function(player, dtime)
	local st = L.player_state(player:get_player_name())
	local pos = player:get_pos()
	local feet = core.get_node(vector.round(vector.offset(pos, 0, 0.2, 0))).name
	local head = core.get_node(vector.round(vector.offset(pos, 0, 1.6, 0))).name
	local in_water = is_water(feet)
	local vel = player:get_velocity()
	local pname = player:get_player_name()

	-- Falling into water: one big splash
	if in_water and not st.was_in_water and vel.y < -3 then
		afx_ambience.play("afx_plop", {pos = pos, to_player = pname,
			gain = math.min(1, 0.4 + (-vel.y) / 15), pitch = L.rand(0.6, 0.75),
			max_hear_distance = 24}, true)
	end
	-- Wading: feet in water, head out, moving
	st.wade_t = (st.wade_t or 0) + dtime
	local speed = math.sqrt(vel.x * vel.x + vel.z * vel.z)
	if in_water and not is_water(head) and speed > 1 and st.wade_t > 0.55 then
		st.wade_t = 0
		afx_ambience.play("afx_plop", {pos = pos, to_player = pname,
			gain = L.rand(0.15, 0.3), pitch = L.rand(0.95, 1.25), max_hear_distance = 12}, true)
	end
	st.was_in_water = in_water
end)
