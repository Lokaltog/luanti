-- Your body in the world: air rushing past when you fall or ride fast,
-- breath escaping under water, brushing through tall plants, splashy steps
-- on wet ground, and the things in your hand.

local A = afx_ambience
local L = A.lib

-- Per-player non-positional loops, faded to a target level
local loops = {}

local function loop_to(pname, key, sound, gain, pitch, fade)
	loops[pname] = loops[pname] or {}
	local l = loops[pname][key]
	if gain < 0.01 then
		if l then
			core.sound_fade(l.handle, fade or 0.6, 0)
			loops[pname][key] = nil
		end
		return
	end
	if not l then
		local h = core.sound_play(sound, {to_player = pname, loop = true, gain = 0.01,
			pitch = pitch or 1})
		if not h then return end
		core.sound_fade(h, fade or 0.6, gain)
		loops[pname][key] = {handle = h, gain = gain}
	elseif math.abs(l.gain - gain) > 0.03 then
		core.sound_fade(l.handle, fade or 0.6, gain)
		l.gain = gain
	end
end

core.register_on_leaveplayer(function(p) loops[p:get_player_name()] = nil end)

local function walkable(name)
	local d = core.registered_nodes[name]
	return d and d.walkable
end

-- Tall, soft plants you push through
local function brushy(name)
	local d = core.registered_nodes[name]
	if not d or d.walkable then return false end
	local g = d.groups or {}
	return d.drawtype == "plantlike" or d.drawtype == "plantlike_rooted" or
		(g.grass or 0) > 0 or (g.flora or 0) > 0 or (g.flower or 0) > 0 or
		(g.plant or 0) > 0 or (g.leaves or 0) > 0 or (g.double_plant or 0) > 0
end

-- From how far the player actually moved since the last tick: covers
-- walking, falling, minecarts, boats and anything that teleports them along.
local function speed_of(player, st, dtime)
	local pos = player:get_pos()
	local v = vector.zero()
	if st.last_pos and dtime > 0 then
		v = vector.divide(vector.subtract(pos, st.last_pos), dtime)
		if vector.length(v) > 70 then v = vector.zero() end -- a teleport
	end
	st.last_pos = pos
	return v, vector.length(v)
end

A.register_tick(function(player, dtime)
	local pname = player:get_player_name()
	local st = L.player_state(pname)
	local pos = player:get_pos()
	local now = core.get_us_time() / 1e6
	local vel, speed = speed_of(player, st, dtime)
	local hspeed = math.sqrt(vel.x * vel.x + vel.z * vel.z)

	-- Rushing air: grows with speed past a run (falling, minecarts, flying)
	local rush = math.max(0, math.min(1, (speed - 7) / 22)) ^ 1.3
	loop_to(pname, "rush", "afx_dry_rush", 0.9 * rush, 0.9 + math.min(0.3, speed / 90), 1.5)

	-- Under water: the breath goes, a few bubbles at a time
	local head = core.get_node(vector.round(vector.offset(pos, 0, 1.6, 0))).name
	local breath = player:get_breath()
	if L.CFG.debug and st.last_breath and breath ~= st.last_breath then
		L.log(pname .. " breath " .. breath)
	end
	if core.get_item_group(head, "water") > 0 then
		if st.last_breath and breath < st.last_breath then
			core.sound_play("afx_bubbles", {pos = vector.offset(pos, 0, 1.8, 0), to_player = pname,
				gain = breath <= 3 and 0.7 or 0.4, pitch = L.rand(0.9, 1.15),
				max_hear_distance = 8}, true)
		end
	end
	st.last_breath = breath

	-- Moving through plants: a rustle per stride, louder in tall growth
	local feet = core.get_node(vector.round(vector.offset(pos, 0, 0.3, 0))).name
	local body = core.get_node(vector.round(vector.offset(pos, 0, 1.2, 0))).name
	local depth = (brushy(feet) and 1 or 0) + (brushy(body) and 1 or 0)
	st.brush_t = (st.brush_t or 0) + dtime
	if depth > 0 and hspeed > 1.2 and st.brush_t > math.max(0.25, 1.4 / hspeed) then
		st.brush_t = 0
		core.sound_play("afx_leaf_rustle", {pos = vector.offset(pos, 0, 0.6 * depth, 0),
			to_player = pname, gain = math.min(0.6, 0.12 * depth * hspeed),
			pitch = L.rand(1.05, 1.35), max_hear_distance = 10}, true)
	end

	-- Wet ground: a little squelch under each step outdoors after rain
	local below = core.get_node(vector.round(vector.offset(pos, 0, -0.6, 0))).name
	local wet = A.wetness and A.wetness(now) or 0
	st.wet_d = (st.wet_d or 0) + hspeed * dtime
	if wet > 0.2 and hspeed > 1.2 and walkable(below) and st.wet_d > 1.6 and
			L.sky_open(vector.round(vector.offset(pos, 0, 1, 0))) then
		st.wet_d = 0
		core.sound_play("afx_plop", {pos = pos, to_player = pname,
			gain = 0.1 + 0.15 * wet, pitch = L.rand(1.5, 1.9), max_hear_distance = 8}, true)
	end

	-- In your hand: a pocket clock ticks, a shell holds the sea
	local item = player:get_wielded_item():get_name()
	loop_to(pname, "tick", "afx_dry_tick", item:find("clock") and 0.5 or 0, 1, 2)
	loop_to(pname, "shell", "afx_dry_seashell",
		(item:find("shell") or item:find("conch")) and 0.6 or 0, 1, 0.5)
end)
