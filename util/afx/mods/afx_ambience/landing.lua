-- Things land: a dropped item ticks onto stone, taps dirt, knocks hollow on
-- planks over a drop; an arrow thunks into what it hits, wood or stone.
-- Small, short, positional accents (the engine rings them in the room).

local A = afx_ambience
local L = A.lib

local function sounds_of(name)
	local def = core.registered_nodes[name]
	return def and def.sounds or {}
end

local function sound_name(spec)
	if type(spec) == "table" then return spec.name end
	return spec
end

-- Over empty space (a bridge, a floor above a cellar)?
local function hollow_under(pos)
	local below = vector.offset(pos, 0, -1, 0)
	local n = core.get_node_or_nil(below)
	local d = n and core.registered_nodes[n.name]
	return d and not d.walkable
end

-- A chest broken open drops twenty things at once: a few clatter, not all
local budget_t, budget_n, recent = 0, 0, {}

-- The impact of something small on node `p`, at `speed` nodes/s
local function impact(p, speed, small)
	local now = core.get_us_time() / 1e6
	if now - budget_t > 0.1 then budget_t, budget_n = now, 0 end
	if budget_n >= 4 then return end
	local key = core.hash_node_position(p)
	if now - (recent[key] or -1) < 0.15 then return end
	budget_n = budget_n + 1
	recent[key] = now
	if budget_n == 1 and math.random() < 0.05 then
		for k, t in pairs(recent) do if now - t > 1 then recent[k] = nil end end
	end
	local node = core.get_node_or_nil(p)
	if not node then return end
	local def = core.registered_nodes[node.name]
	if not def or not def.walkable then return end
	local s = sounds_of(node.name)
	-- the node's own footstep, short and light, is how it sounds to be hit
	local name = sound_name(s.footstep) or sound_name(s.dig)
	if not name or name == "" or name == "__group" then
		-- a game that gave the node no sounds: go by what it is
		local g = function(x) return core.get_item_group(node.name, x) > 0 end
		name = (g("wood") or g("tree")) and "default_wood_footstep" or
			(g("stone") or g("cracky")) and "default_hard_footstep" or
			(g("sand")) and "default_sand_footstep" or "default_dirt_footstep"
	end
	local k = math.min(1, (speed - 1.5) / 8)
	if k <= 0 then return end
	local hollow = core.get_item_group(node.name, "wood") > 0 and hollow_under(p)
	A.play(name, {pos = vector.offset(p, 0, 0.5, 0), gain = (small and 0.18 or 0.3) * (0.4 + 0.6 * k),
		pitch = (small and L.rand(1.35, 1.6) or L.rand(1.1, 1.25)) * (hollow and 0.8 or 1),
		max_hear_distance = 14}, true)
	if hollow then
		-- and the boards ring underneath
		A.play("afx_knock_hollow", {pos = vector.offset(p, 0, 0.3, 0), gain = 0.12 * k,
			pitch = 1.5, max_hear_distance = 12}, true)
	end
end

core.register_on_mods_loaded(function()
	-- Dropped items
	local item = core.registered_entities["__builtin:item"]
	if item then
		local step = item.on_step
		item.on_step = function(self, dtime, moveresult, ...)
			if moveresult and moveresult.collisions then
				for _, c in ipairs(moveresult.collisions) do
					if c.type == "node" and c.axis == "y" and c.old_velocity.y < -2 then
						impact(c.node_pos, -c.old_velocity.y, true)
						break
					end
				end
			end
			return step(self, dtime, moveresult, ...)
		end
	end
	-- Arrows sticking in things (Mineclonia/VoxeLibre arrows keep _stuck)
	for name, def in pairs(core.registered_entities) do
		if name:find("arrow") and def.on_step then
			local step = def.on_step
			def.on_step = function(self, ...)
				local was = self._stuck
				local r = step(self, ...)
				if self._stuck and not was and self._stuckin then
					impact(self._stuckin, 14, false)
				end
				return r
			end
		end
	end
end)
