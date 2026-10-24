-- Your body in the world: air rushing past when you fall or ride fast,
-- breath escaping under water, brushing through tall plants, splashy steps
-- on wet ground, and the things in your hand.

local A = afx_ambience
local L = A.lib

-- Per-player non-positional loops, faded to a target level
local loops = {}

local function loop_to(pname, key, sound, gain, pitch, fade)
	gain = A.gain_for(sound, gain, pname)
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

-- Ground that gets muddy: dirt, grass, sand; not planks, stone or roofs
local function soil(name)
	local d = core.registered_nodes[name]
	if not d or not d.walkable then return false end
	local g = d.groups or {}
	if (g.stone or 0) > 0 or (g.wood or 0) > 0 or (g.tree or 0) > 0 then return false end
	return (g.soil or 0) > 0 or (g.dirt or 0) > 0 or (g.sand or 0) > 0 or
		(g.grass_block or 0) > 0 or name:find("dirt") ~= nil or name:find("grass") ~= nil or
		name:find("mud") ~= nil
end

-- Tall, soft plants you push through (not kelp under water, cobwebs,
-- torches, rails or carpets of moss: real growth)
local brushy_cache = {}
local function brushy(name)
	local b = brushy_cache[name]
	if b ~= nil then return b end
	local d = core.registered_nodes[name]
	local g = d and d.groups or {}
	b = d ~= nil and not d.walkable and d.drawtype == "plantlike" and d.liquidtype == "none" and
		((g.grass or 0) > 0 or (g.flora or 0) > 0 or (g.flower or 0) > 0 or
		(g.plant or 0) > 0 or (g.double_plant or 0) > 0 or (g.leaves or 0) > 0) and
		not name:find("cobweb") and not name:find("web")
	brushy_cache[name] = b
	return b
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
			afx_ambience.play("afx_bubbles", {pos = vector.offset(pos, 0, 1.8, 0), to_player = pname,
				gain = breath <= 3 and 0.7 or 0.4, pitch = L.rand(0.9, 1.15),
				max_hear_distance = 8}, true)
		end
	end
	st.last_breath = breath

	-- Moving through plants: a soft rustle every couple of strides, a little
	-- more in tall growth. It sits right by the ear, so it stays well under
	-- the footsteps (the game's grass step is ~0.13).
	local feet = core.get_node(vector.round(vector.offset(pos, 0, 0.3, 0))).name
	local body = core.get_node(vector.round(vector.offset(pos, 0, 1.2, 0))).name
	local depth = (brushy(feet) and 1 or 0) + (brushy(body) and 1 or 0)
	st.brush_d = (st.brush_d or 0) + hspeed * dtime
	if depth > 0 and hspeed > 1.2 and st.brush_d > 1.8 then
		st.brush_d = 0
		-- non-positional: a positional sound this close to the ear clips
		-- (the engine's x3 for positional sounds hits AL_MAX_GAIN)
		afx_ambience.play("afx_leaf_rustle", {to_player = pname,
			gain = math.min(0.14, 0.028 * depth * hspeed),
			pitch = L.rand(1.05, 1.35)}, true)
	elseif depth == 0 then
		st.brush_d = 0
	end

	-- Wet ground: a little squelch under each step outdoors after rain
	local below = core.get_node(vector.round(vector.offset(pos, 0, -0.6, 0))).name
	local wet = A.wetness and A.wetness(now, pname) or 0
	st.wet_d = (st.wet_d or 0) + hspeed * dtime
	if wet > 0.2 and hspeed > 1.2 and soil(below) and st.wet_d > 1.6 and
			L.sky_open(vector.round(vector.offset(pos, 0, 1, 0))) then
		st.wet_d = 0
		afx_ambience.play("afx_plop", {pos = pos, to_player = pname,
			gain = 0.1 + 0.15 * wet, pitch = L.rand(1.5, 1.9), max_hear_distance = 8}, true)
	end

	-- In your hand: a pocket clock ticks, a shell holds the sea
	local item = player:get_wielded_item():get_name()
	loop_to(pname, "tick", "afx_dry_tick", item:find("clock") and 0.5 or 0, 1, 2)
	loop_to(pname, "shell", "afx_dry_seashell",
		(item:find("nautilus") or item:find("conch") or item:find("seashell")) and 0.6 or 0, 1, 0.5)
end)
