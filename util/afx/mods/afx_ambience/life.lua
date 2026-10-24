-- The living world reacting to you: birds burst out of a tree when you
-- break its leaves, frogs go quiet and plop into the water as you come
-- near, croaks from the shore at night, and the hum of a village.

local A = afx_ambience
local L = A.lib

local ALARM = {"afx_bird_goldfinch", "afx_bird_bluebird", "afx_bird_robin"}

--[[ Birds flushing from leaves ]]

local last_flush = {}

local function flush(pos, pname)
	local now = core.get_us_time() / 1e6
	if last_flush[pname] and now - last_flush[pname] < 6 then return end
	local tod = core.get_timeofday()
	if tod < 0.22 or tod > 0.78 then return end -- asleep
	local leaves = core.find_nodes_in_area(vector.offset(pos, -6, -3, -6),
			vector.offset(pos, 6, 6, 6), {"group:leaves"})
	if #leaves < 12 then return end -- a lone bush has no birds in it
	if A.rain_at(pos) > 0 then return end
	last_flush[pname] = now

	core.sound_play("afx_leaf_rustle", {pos = pos, to_player = pname, gain = 0.8,
		pitch = L.rand(0.9, 1.1), max_hear_distance = 24}, true)
	-- two to four birds, off and away, up and outward from the tree
	local n = math.random(2, 4)
	local away = {x = L.rand(-1, 1), y = 0, z = L.rand(-1, 1)}
	for i = 1, n do
		core.after(0.08 + i * L.rand(0.08, 0.2), function()
			local p = vector.add(pos, vector.multiply(away, 2 + i * 1.5))
			p = vector.offset(p, L.rand(-1.5, 1.5), 1 + i * 0.8, L.rand(-1.5, 1.5))
			core.sound_play(L.pick(ALARM), {pos = p, to_player = pname,
				gain = L.rand(0.5, 0.8), pitch = L.rand(1.05, 1.25), max_hear_distance = 32}, true)
			if i == 1 then
				core.sound_play("afx_leaf_rustle", {pos = p, to_player = pname, gain = 0.5,
					pitch = 1.2, max_hear_distance = 20}, true)
			end
		end)
	end
	-- and the tree goes quiet for a while
	local st = L.player_state(pname)
	st.next_bird = math.max(st.next_bird or 0, now + 40)
end

core.register_on_dignode(function(pos, oldnode, digger)
	if not L.CFG.birds or not digger or not digger:is_player() then return end
	if core.get_item_group(oldnode.name, "leaves") > 0 then
		flush(pos, digger:get_player_name())
	end
end)

core.register_on_punchnode(function(pos, node, puncher)
	if not L.CFG.birds or not puncher or not puncher:is_player() then return end
	-- a few punches on a trunk shake the birds out too
	if core.get_item_group(node.name, "tree") > 0 and math.random() < 0.25 then
		flush(vector.offset(pos, 0, 3, 0), puncher:get_player_name())
	end
end)

--[[ Frogs ]]

local function frog_time(ctx)
	return ctx.tod < 0.24 or ctx.tod > 0.74 or ctx.rain > 0
end

A.register_layer(function(ctx, wanted)
	if ctx.dimension ~= "overworld" or ctx.underground then return end
	local shore = ctx.shore
	if not shore or #shore < 4 then return end
	local st = ctx.st
	local now = ctx.now

	-- Coming close to the water's edge: the nearest frogs plop in and the
	-- chorus stops for a while
	local near
	for _, p in ipairs(shore) do
		if vector.distance(p, ctx.ppos) < 4 then near = p break end
	end
	if near and (not st.frog_quiet_until or now > st.frog_quiet_until) and
			frog_time(ctx) then
		st.frog_quiet_until = now + 25
		for i = 1, math.random(1, 3) do
			local p = L.pick(shore)
			if vector.distance(p, ctx.ppos) < 8 then
				core.after(i * L.rand(0.1, 0.35), function()
					core.sound_play("afx_plop", {pos = p, to_player = ctx.pname,
						gain = L.rand(0.25, 0.5), pitch = L.rand(1.1, 1.5), max_hear_distance = 16}, true)
				end)
			end
		end
	end
	ctx.frogs_quiet = st.frog_quiet_until and now < st.frog_quiet_until

	-- Croaks from the far bank at night, when they're not hiding from you
	if frog_time(ctx) and not ctx.frogs_quiet and now >= (st.next_croak or 0) then
		st.next_croak = now + L.rand(3, 14)
		for _ = 1, 4 do
			local p = L.pick(shore)
			if vector.distance(p, ctx.ppos) > 6 then
				core.sound_play("afx_frog", {pos = p, to_player = ctx.pname,
					gain = L.rand(0.3, 0.7), pitch = L.rand(0.85, 1.15), max_hear_distance = 32}, true)
				break
			end
		end
	end
end)

--[[ Villages: people and animals about by day, a quiet village at night ]]

A.register_layer(function(ctx, wanted)
	if ctx.dimension ~= "overworld" or ctx.underground then return end
	local villagers = 0
	for _, obj in ipairs(core.get_objects_inside_radius(ctx.ppos, 40)) do
		local ent = obj:get_luaentity()
		if ent and ent.name and ent.name:find("villager") then
			villagers = villagers + 1
		end
	end
	if villagers < 2 then return end
	local k = math.min(1, villagers / 6)
	wanted.v_day = {sound = "afx_out_village_day", gain = 0.5 * k * ctx.day, fade = 0.1}
	wanted.v_night = {sound = "afx_out_village_night", gain = 0.5 * k * (1 - ctx.day), fade = 0.1}
end)
