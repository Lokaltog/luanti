-- Nature's small movements, kept in the background:
--   gusts you hear coming: a gust hisses through the trees upwind, sweeps
--     toward you and past (a moving, invisible emitter)
--   crickets fall silent: a few crickets in the grass around you at night,
--     each going quiet as you come near and starting again behind you
--   singing ice: a frozen lake pings and moans now and then, more at dusk
--   hearing yourself: somewhere really quiet, your clothes rustle as you
--     move, and after a run your breath settles

local A = afx_ambience
local L = A.lib

--[[ Gusts you hear coming ]]

core.register_entity("afx_ambience:gust", {
	initial_properties = {
		physical = false, collide_with_objects = false, pointable = false,
		visual = "sprite", textures = {"blank.png"}, is_visible = false,
		static_save = false,
	},
	on_activate = function(self)
		self.t = 0
	end,
	on_step = function(self, dtime)
		self.t = self.t + dtime
		if self.t > (self.life or 7) then
			if self.handle then core.sound_fade(self.handle, 1, 0) end
			self.object:remove()
		end
	end,
})

local function sweep(player, dir, peak)
	local ppos = player:get_pos()
	local speed = 8 + 4 * peak -- nodes/s, faster in a stronger gust
	local life = 7
	-- start upwind so it reaches you about halfway through
	local start = vector.offset(ppos, -dir.x * speed * life * 0.5, 5, -dir.z * speed * life * 0.5)
	local obj = core.add_entity(start, "afx_ambience:gust")
	if not obj then return end
	obj:set_velocity({x = dir.x * speed, y = 0, z = dir.z * speed})
	local ent = obj:get_luaentity()
	ent.life = life
	ent.handle = A.play("afx_gust", {object = obj, to_player = player:get_player_name(),
		gain = 0.22 + 0.15 * math.min(1, peak), pitch = L.rand(0.92, 1.08), max_hear_distance = 48})
end

-- Called by weather.lua when a gust rises
function A.on_gust(peak)
	if not L.CFG.beds then return end
	local d = A.wind_override and A.wind_override.dir or A.wind_dir()
	for _, player in ipairs(core.get_connected_players()) do
		local st = L.player_state(player:get_player_name())
		local ctx_ok = st.last_ctx and not st.last_ctx.underground and not st.last_ctx.indoors and
				st.last_ctx.dimension == "overworld" and (st.last_ctx.rain or 0) < 0.6
		-- only where trees catch it, and not every gust
		if ctx_ok and (st.trees or 0) >= 40 and math.random() < 0.6 then
			sweep(player, d, peak or 1)
		end
	end
end

--[[ Crickets around you, falling silent as you pass ]]

local function cricket_spots(ctx)
	local st = ctx.st
	if st.ck_spots and vector.distance(st.ck_at, ctx.ppos) < 10 and ctx.now - st.ck_t < 40 then
		return st.ck_spots
	end
	local grass = core.find_nodes_in_area(vector.offset(ctx.ppos, -16, -3, -16),
			vector.offset(ctx.ppos, 16, 3, 16), {"group:grass", "group:flora", "group:flower"})
	local spots = {}
	for _ = 1, 30 do
		if #spots >= 5 or #grass == 0 then break end
		local p = grass[math.random(#grass)]
		local d = vector.distance(p, ctx.ppos)
		local far_apart = true
		for _, q in ipairs(spots) do
			if vector.distance(p, q.pos) < 5 then far_apart = false break end
		end
		if d > 5 and far_apart then spots[#spots + 1] = {pos = p, quiet_until = 0} end
	end
	st.ck_spots, st.ck_at, st.ck_t = spots, ctx.ppos, ctx.now
	return spots
end

A.register_layer(function(ctx, wanted)
	ctx.st.last_ctx = ctx
	if ctx.dimension ~= "overworld" or ctx.underground or ctx.indoors then return end
	local night = ctx.tod < 0.22 or ctx.tod > 0.8
	if not night or ctx.rain > 0 or ctx.snow > 0 then return end
	local spots = cricket_spots(ctx)
	ctx.crickets_near = #spots
	for _, s in ipairs(spots) do
		local d = vector.distance(s.pos, ctx.ppos)
		if d < 4.5 then
			-- too close: it stops, and waits a while after you've gone
			s.quiet_until = ctx.now + L.rand(5, 12)
		end
		if ctx.now >= s.quiet_until and d >= 6 then
			wanted["ck" .. core.hash_node_position(s.pos)] = {sound = "afx_cricket", pos = s.pos,
				gain = 0.1, range = 20, fade = d < 8 and 0.08 or 0.3}
		end
	end
end)

--[[ Singing ice ]]

local ICE = {}
core.register_on_mods_loaded(function()
	for name, def in pairs(core.registered_nodes) do
		local g = def.groups or {}
		if (g.ice or 0) > 0 or (name:find("ice") and not name:find("dice") and not name:find("slice")
				and def.drawtype == "normal") then
			ICE[#ICE + 1] = name
		end
	end
end)

A.register_layer(function(ctx, wanted)
	if #ICE == 0 or ctx.dimension ~= "overworld" or ctx.underground then return end
	local st = ctx.st
	if ctx.now < (st.next_ice or 0) then return end
	local ice = core.find_nodes_in_area(vector.offset(ctx.ppos, -20, -4, -20),
			vector.offset(ctx.ppos, 20, 2, 20), ICE)
	if #ice < 40 then
		st.next_ice = ctx.now + 10
		return
	end
	-- the ice talks most as it cools at dusk and in the night cold
	local dusk = (ctx.tod > 0.72 and ctx.tod < 0.85) and 0.4 or (ctx.day < 0.5 and 0.7 or 1)
	st.next_ice = ctx.now + L.rand(7, 26) * dusk
	local p
	for _ = 1, 8 do
		local q = ice[math.random(#ice)]
		local d = vector.distance(q, ctx.ppos)
		if d > 5 and d < 22 and L.sky_open(vector.offset(q, 0, 1, 0)) then p = q break end
	end
	if not p then return end
	A.play("afx_ice_sing", {pos = p, to_player = ctx.pname, gain = L.rand(0.15, 0.32),
		pitch = L.rand(0.85, 1.15), max_hear_distance = 48}, true)
end)

--[[ Hearing yourself where it's quiet ]]

A.register_tick(function(player, dtime)
	local pname = player:get_player_name()
	local st = L.player_state(pname)
	local ctx = st.last_ctx
	if not ctx then return end
	-- quiet: in a room or a cave, nothing going on outside, no rain, few
	-- sounds about (the ambience itself is near silent)
	local n = 0
	for _ in pairs(st.emitters or {}) do n = n + 1 end
	local quiet = (ctx.indoors or ctx.underground or (ctx.hearing and (ctx.hearing.enclosure or 0) > 0.8)) and
			(ctx.rain or 0) == 0 and (ctx.exposure or 0) < 0.12 and n <= 2 and not ctx.underwater
	-- (from how far you moved: get_velocity is zero while riding or when
	-- moved by the game)
	local pos = player:get_pos()
	local hspeed = 0
	if st.nat_last and dtime > 0 then
		local dx, dz = pos.x - st.nat_last.x, pos.z - st.nat_last.z
		hspeed = math.sqrt(dx * dx + dz * dz) / dtime
		if hspeed > 40 then hspeed = 0 end
	end
	st.nat_last = pos
	local now = core.get_us_time() / 1e6
	-- running builds up breath to settle afterwards
	if hspeed > 5 then
		st.exert = math.min(1, (st.exert or 0) + dtime / 6)
	end
	if not quiet then return end
	st.cloth_d = (st.cloth_d or 0) + hspeed * dtime
	if hspeed > 1 and st.cloth_d > 2.4 then
		st.cloth_d = 0
		if math.random() < 0.6 then
			A.play("afx_cloth", {to_player = pname, gain = L.rand(0.03, 0.06),
				pitch = L.rand(0.9, 1.1)}, true)
		end
	end
	if hspeed < 1 and (st.exert or 0) > 0.3 and now >= (st.next_breath or 0) then
		st.next_breath = now + L.rand(3.2, 4.2)
		A.play("afx_dry_breath", {to_player = pname, gain = 0.05 * st.exert,
			pitch = L.rand(0.95, 1.05)}, true)
		st.exert = st.exert - 0.2
	end
end)
