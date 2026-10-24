-- The cinematic touches: a heartbeat when it matters, and two opt-in
-- extras: something that follows you in the deep dark, and the faint
-- shimmer of treasure in the rock.

local A = afx_ambience
local L = A.lib

local FOLLOWER = core.settings:get_bool("afx_ambience_follower") == true
local TREASURE = core.settings:get_bool("afx_ambience_treasure") == true

--[[ Heartbeat: low health, or something hostile near you in the dark ]]

local beats = {}

local function hostile_near(player, pos, r)
	local best
	for _, obj in ipairs(core.get_objects_inside_radius(pos, r)) do
		local ent = obj:get_luaentity()
		if ent and (ent.type == "monster" or ent.hostile or ent._hostile or
				(ent.name and (ent.name:find("zombie") or ent.name:find("skeleton") or
				ent.name:find("spider") or ent.name:find("creeper")))) then
			local d = vector.distance(obj:get_pos(), pos)
			if not best or d < best then best = d end
		end
	end
	return best
end

A.register_tick(function(player, dtime)
	local pname = player:get_player_name()
	local pos = player:get_pos()
	local hp = player:get_hp()
	local hp_max = (player:get_properties().hp_max or 20)
	local intensity = 0
	if core.settings:get_bool("enable_damage") ~= false and hp > 0 then
		intensity = math.max(0, (0.35 * hp_max - hp) / (0.35 * hp_max))
	end
	local light = core.get_node_light(vector.round(vector.offset(pos, 0, 1, 0))) or 15
	if light <= 6 then
		local d = hostile_near(player, pos, 10)
		if d then intensity = math.max(intensity, 0.35 + 0.45 * (1 - d / 10)) end
	end
	local b = beats[pname]
	if intensity > 0.05 then
		local gain = 0.25 + 0.5 * intensity
		local pitch = 1.0 + 0.6 * intensity -- 60 to 96 beats a minute
		if not b then
			local h = core.sound_play("afx_dry_heart", {to_player = pname, loop = true,
				gain = 0.01, pitch = pitch})
			if h then
				core.sound_fade(h, 0.4, gain)
				beats[pname] = {handle = h, gain = gain, pitch = pitch}
			end
		elseif math.abs(b.gain - gain) > 0.05 or math.abs(b.pitch - pitch) > 0.08 then
			-- pitch is fixed per play: restart at the new tempo
			core.sound_stop(b.handle)
			local h = core.sound_play("afx_dry_heart", {to_player = pname, loop = true,
				gain = gain, pitch = pitch})
			beats[pname] = h and {handle = h, gain = gain, pitch = pitch} or nil
		end
	elseif b then
		core.sound_fade(b.handle, 0.3, 0)
		beats[pname] = nil
	end
end)

core.register_on_leaveplayer(function(p) beats[p:get_player_name()] = nil end)

--[[ The follower (opt-in): steps behind you that stop when you stop ]]

local function footstep_of(pos)
	local n = core.get_node(vector.round(vector.offset(pos, 0, -0.6, 0)))
	local def = core.registered_nodes[n.name]
	local fs = def and def.sounds and def.sounds.footstep
	if type(fs) == "table" then fs = fs.name end
	if not fs or fs == "" then fs = "default_hard_footstep" end -- shipped takes
	return fs
end

if FOLLOWER then
	A.register_tick(function(player, dtime)
		local st = L.player_state(player:get_player_name())
		local now = core.get_us_time() / 1e6
		local pos = player:get_pos()
		local ear = vector.round(vector.offset(pos, 0, 1.6, 0))
		local deep = (core.get_node_light(ear, 0.5) or 15) <= 2 and pos.y < -10
		if not deep then st.follow_until = nil return end
		-- once in a long while, for twenty seconds
		if not st.follow_until then
			st.next_follow = st.next_follow or now + L.rand(120, 400)
			if now < st.next_follow then return end
			st.follow_until = now + 20
			st.next_follow = now + L.rand(300, 900)
		end
		if now > st.follow_until then st.follow_until = nil return end
		-- walking: a step behind yours, a beat late
		local v = st.follow_last and vector.subtract(pos, st.follow_last) or vector.zero()
		st.follow_last = pos
		local moving = vector.length(v) / dtime > 1.0
		st.follow_d = (st.follow_d or 0) + vector.length(v)
		if moving and st.follow_d > 1.7 then
			st.follow_d = 0
			local back = vector.normalize({x = -v.x, y = 0, z = -v.z})
			local p = vector.add(pos, vector.multiply(back, L.rand(5, 7)))
			local name = footstep_of(pos)
			if name then
				core.after(0.25, function()
					core.sound_play(name, {pos = p, to_player = player:get_player_name(),
						gain = 0.45, pitch = L.rand(0.85, 0.95), max_hear_distance = 16}, true)
				end)
			end
		end
	end)
end

--[[ Treasure shimmer (opt-in): rare ore close by glints faintly ]]

local RARE = {}
core.register_on_mods_loaded(function()
	for name in pairs(core.registered_nodes) do
		if (name:find("diamond") or name:find("emerald") or name:find("ancient_debris")) and
				(name:find("ore") or name:find("debris")) then
			RARE[#RARE + 1] = name
		end
	end
end)

if TREASURE then
	A.register_layer(function(ctx, wanted)
		if #RARE == 0 or not ctx.underground then return end
		local st, now = ctx.st, ctx.now
		if now < (st.next_glint or 0) then return end
		local ore = core.find_nodes_in_area(vector.subtract(ctx.ppos, 5),
			vector.add(ctx.ppos, 5), RARE)
		if #ore == 0 then return end
		st.next_glint = now + L.rand(3, 7)
		local p = ore[math.random(#ore)]
		core.sound_play("afx_bowl", {pos = p, to_player = ctx.pname, gain = 0.12,
			pitch = L.rand(3.6, 4.0), max_hear_distance = 8}, true)
	end)
end
