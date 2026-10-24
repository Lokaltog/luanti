-- Living creatures: animals notice you. Run at a flock of chickens, land
-- hard next to the sheep, and they startle: a squawk and a flurry of wings,
-- a bleat, a pig's grunt, the rabbit's thump. Calm animals stay calm if you
-- walk. (Footsteps sized to the body are the engine's: a chicken patters,
-- a cow plods.)
--
-- Audio only: the game decides where they actually go. What kind of animal
-- and its voice come from the server's entity table (afx_ambience's
-- bridge sends the game's mob definitions).

local A = afx_ambience
local L = A.lib

-- Passive animals, by name: their own voice, and what their body does
local KINDS = {
	{"chicken", flap = true}, {"parrot", flap = true}, {"bat", flap = true},
	{"sheep"}, {"cow"}, {"mooshroom"}, {"pig"}, {"rabbit", thump = true},
	{"horse", hooves = true}, {"donkey", hooves = true}, {"mule", hooves = true},
	{"llama"}, {"goat"}, {"fox"}, {"cat"}, {"ocelot"}, {"turtle"}, {"frog"},
}

local function kind_of(ent)
	if not ent or not ent.name then return nil end
	if ent.type and ent.type ~= "animal" and ent.type ~= "npc" then return nil end -- monsters don't startle
	if ent.name:find("zombi") or ent.name:find("piglin") or ent.passive == false then return nil end
	for _, k in ipairs(KINDS) do
		if ent.name:find(k[1], 1, true) then return k end
	end
end

local function voice_of(ent)
	local s = ent.child and ent.sounds_child or ent.sounds
	if type(s) ~= "table" then return nil end
	local v = s.random or s.damage
	if type(v) == "table" then v = v.name end
	if type(v) ~= "string" then return nil end
	return v, (s.base_pitch or 1) * (ent.child and not ent.sounds_child and 1.5 or 1)
end

local last_startle = {} -- by object: seconds

local function startle(obj, ent, k, pname, now, strength)
	local key = obj._id or tostring(obj) -- (a fresh wrapper each scan: by id)
	if now - (last_startle[key] or -100) < 8 then return end
	last_startle[key] = now
	local pos = obj:get_pos()
	if not pos then return end
	local voice, pitch = voice_of(ent)
	local delay = L.rand(0.05, 0.35)
	core.after(delay, function()
		if not obj:get_pos() then return end
		local p = obj:get_pos()
		if voice then
			-- startled: a little higher, a little louder, cut short
			A.play(voice, {pos = vector.offset(p, 0, 0.5, 0), to_player = pname,
				gain = 0.5 + 0.4 * strength, pitch = pitch * L.rand(1.08, 1.2),
				max_hear_distance = 24}, true)
		end
		if k.flap then
			-- a flurry of wings: quick rustles, rising off the ground
			for i = 0, 3 do
				core.after(i * L.rand(0.07, 0.12), function()
					A.play("afx_leaf_rustle", {pos = vector.offset(p, 0, 0.4 + 0.2 * i, 0),
						to_player = pname, gain = 0.25 * strength, pitch = L.rand(1.8, 2.3),
						max_hear_distance = 16}, true)
				end)
			end
		elseif k.thump then
			A.play("afx_knock_hollow", {pos = p, to_player = pname, gain = 0.25 * strength,
				pitch = 2.2, max_hear_distance = 12}, true)
		elseif k.hooves then
			-- a stamp and a snort of the hooves on whatever's under them
			local n = core.get_node(vector.round(vector.offset(p, 0, -0.6, 0)))
			local def = core.registered_nodes[n.name]
			local fs = def and def.sounds and def.sounds.footstep
			local name = type(fs) == "table" and fs.name or fs
			if name and name ~= "" then
				for i = 0, 2 do
					core.after(0.1 + i * 0.16, function()
						A.play(name, {pos = p, to_player = pname, gain = 0.5 * strength,
							pitch = 0.75, max_hear_distance = 20}, true)
					end)
				end
			end
		end
	end)
end

-- What the player does that an animal notices: running at it, landing
-- hard, a sudden sprint
A.register_tick(function(player, dtime)
	local pname = player:get_player_name()
	local st = L.player_state(pname)
	local now = core.get_us_time() / 1e6
	local vel = player:get_velocity()
	local hspeed = math.sqrt(vel.x * vel.x + vel.z * vel.z)
	local landed = (st.cr_last_vy or 0) < -7 and vel.y > -1
	st.cr_last_vy = vel.y
	local running = hspeed > 5.2
	if not (landed or running) then return end
	if now < (st.cr_next or 0) then return end
	st.cr_next = now + 0.6
	local pos = player:get_pos()
	local n = 0
	for _, obj in ipairs(core.get_objects_inside_radius(pos, landed and 7 or 5)) do
		local ent = obj:get_luaentity()
		local k = kind_of(ent)
		if k then
			local d = vector.distance(obj:get_pos(), pos)
			-- running: only the ones you're running toward
			local toward = true
			if running and not landed and hspeed > 0 then
				local to = vector.normalize(vector.subtract(obj:get_pos(), pos))
				toward = (to.x * vel.x + to.z * vel.z) / hspeed > 0.3
			end
			if toward then
				startle(obj, ent, k, pname, now, math.max(0.3, 1 - d / 7))
				n = n + 1
				if n >= 3 then break end -- a flock: a few voices, not all of them
			end
		end
	end
end)

core.register_on_mods_loaded(function()
	-- forget mobs that are gone, now and then
	local function sweep()
		local now = core.get_us_time() / 1e6
		for k, t in pairs(last_startle) do if now - t > 30 then last_startle[k] = nil end end
		core.after(30, sweep)
	end
	core.after(30, sweep)
end)

A.add_report(function()
	local names = {}
	for _, k in ipairs(KINDS) do names[#names + 1] = k[1] end
	return {"creatures: startle " .. table.concat(names, " ")}
end)
