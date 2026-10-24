-- fx_ambience: ambient particles for the fx fork.
-- (Particle `size` is in tenths of a node. Textures are premultiplied, black
-- where transparent, and drawn with additive or screen blending.)
--
-- Game agnostic: candidate nodes are found by groups and node properties,
-- never by name. Every second, per player, a few nodes in a box around the
-- player are sampled and short particle spawners (visible to that player
-- only) are started at them. The fx renderer makes glowing particles bloom.
--
--   fireflies  night, above grass/flowers/leaves
--   motes      dust drifting around light sources (torches, lamps)
--   embers     rising sparks above lava and fire
--   drips      water dripping from the underside of blocks below water
--   bubbles    rising in water near the player
--   leaves     falling from leaf blocks with air below (uses the leaf texture)

local S = core.settings
local R, RY = 12, 6           -- search box half size
local INTERVAL = 1.0

local function enabled()
	return S:get_bool("fx_particles", true)
end

local function density()
	return tonumber(S:get("fx_particles_density") or "") or 1.0
end

local light_names = {}
local water_flowing = {}
core.register_on_mods_loaded(function()
	for name, def in pairs(core.registered_nodes) do
		local ls = def.light_source or 0
		if ls >= 8 and (def.liquidtype or "none") == "none"
				and (core.get_item_group(name, "fire") == 0) then
			light_names[#light_names + 1] = name
		end
		if def.liquidtype == "flowing" then
			water_flowing[name] = true
		end
	end
end)

local function pick(list, n)
	local out = {}
	if #list <= n then
		return list
	end
	for _ = 1, n do
		out[#out + 1] = list[math.random(#list)]
	end
	return out
end

local function is_air(pos)
	return core.get_node(pos).name == "air"
end

local function walkable(pos)
	local def = core.registered_nodes[core.get_node(pos).name]
	return def and def.walkable and def.drawtype == "normal"
end

local function night()
	local t = core.get_timeofday()
	return t < 0.21 or t > 0.79
end

local debug = S:get_bool("fx_particles_debug", false)
local spawned = 0

local function spawn(player, def)
	spawned = spawned + 1
	def.playername = player
	def.time = def.time or INTERVAL * 1.1
	core.add_particlespawner(def)
end

local function v(x, y, z) return {x = x, y = y, z = z} end

local feature = {}

function feature.fireflies(name, minp, maxp, d)
	if not night() then return end
	local list = core.find_nodes_in_area_under_air(minp, maxp,
		{"group:flora", "group:flower", "group:grass", "group:leaves", "group:plant"})
	for _, p in ipairs(pick(list, math.ceil(3 * d))) do
		spawn(name, {
			amount = 1,
			pos = {min = v(p.x - 1.5, p.y + 0.6, p.z - 1.5), max = v(p.x + 1.5, p.y + 2.5, p.z + 1.5)},
			vel = {min = v(-0.25, -0.1, -0.25), max = v(0.25, 0.15, 0.25)},
			jitter = {min = v(-0.4, -0.2, -0.4), max = v(0.4, 0.2, 0.4)},
			exptime = {min = 2.5, max = 4.5},
			size = {min = 1.2, max = 2.0},
			texture = {
				name = "fx_ambience_dot.png^[multiply:#c8ff70",
				alpha_tween = {0, 1, 1, 0.8, 0, style = "pulse", reps = 2},
				blend = "add",
			},
			glow = 14,
		})
	end
end

function feature.motes(name, minp, maxp, d)
	if #light_names == 0 then return end
	local list = core.find_nodes_in_area(minp, maxp, light_names)
	for _, p in ipairs(pick(list, math.ceil(2 * d))) do
		spawn(name, {
			amount = math.ceil(3 * d),
			pos = {min = v(p.x - 2, p.y - 1, p.z - 2), max = v(p.x + 2, p.y + 2, p.z + 2)},
			vel = {min = v(-0.05, -0.04, -0.05), max = v(0.05, 0.06, 0.05)},
			jitter = {min = v(-0.05, -0.05, -0.05), max = v(0.05, 0.05, 0.05)},
			exptime = {min = 4, max = 7},
			size = {min = 0.5, max = 0.9},
			texture = {
				name = "fx_ambience_dot.png^[multiply:#fff0d0",
				alpha_tween = {0, 0.5, 0},
				blend = "add",
			},
			glow = 9,
		})
	end
end

function feature.embers(name, minp, maxp, d)
	local list = core.find_nodes_in_area_under_air(minp, maxp, {"group:lava", "group:fire"})
	for _, p in ipairs(pick(list, math.ceil(4 * d))) do
		local fire = core.get_item_group(core.get_node(p).name, "fire") > 0
		local y0 = fire and p.y - 0.2 or p.y + 0.5
		spawn(name, {
			amount = math.ceil((fire and 4 or 2) * d),
			pos = {min = v(p.x - 0.45, y0, p.z - 0.45), max = v(p.x + 0.45, y0 + 0.2, p.z + 0.45)},
			vel = {min = v(-0.3, 0.8, -0.3), max = v(0.3, 2.0, 0.3)},
			acc = v(0, -0.4, 0),
			drag = v(0.6, 0.2, 0.6),
			jitter = {min = v(-0.6, 0, -0.6), max = v(0.6, 0, 0.6)},
			exptime = {min = 0.8, max = 2.2},
			size = {min = 1.0, max = 2.2},
			texture = {
				name = "fx_ambience_spark.png^[multiply:#ffa040",
				alpha_tween = {1, 0},
				scale_tween = {1, 0.3},
				blend = "add",
			},
			glow = 14,
		})
	end
end

function feature.drips(name, minp, maxp, d)
	local list = core.find_nodes_in_area(minp, maxp, {"group:water"})
	local n = 0
	for _, p in ipairs(pick(list, 24)) do
		local below = v(p.x, p.y - 1, p.z)
		local gap = v(p.x, p.y - 2, p.z)
		if walkable(below) and is_air(gap) then
			spawn(name, {
				amount = math.random() < 0.6 * d and 1 or 0,
				pos = {min = v(p.x - 0.4, p.y - 1.52, p.z - 0.4), max = v(p.x + 0.4, p.y - 1.52, p.z + 0.4)},
				acc = v(0, -9.8, 0),
				exptime = 3,
				size = 1.2,
				collisiondetection = true,
				collision_removal = true,
				texture = {name = "fx_ambience_drop.png^[multiply:#a0c8ff", blend = "screen"},
			})
			n = n + 1
			if n >= 3 then break end
		end
	end
end

function feature.bubbles(name, minp, maxp, d, ppos)
	-- only close to the player: bubbles are small
	local m = 5
	local list = core.find_nodes_in_area(v(ppos.x - m, ppos.y - m, ppos.z - m),
		v(ppos.x + m, ppos.y + 2, ppos.z + m), {"group:water"})
	for _, p in ipairs(pick(list, math.ceil(3 * d))) do
		if not water_flowing[core.get_node(p).name] then
			-- rise until the surface: count water above (max 6)
			local depth = 0
			for dy = 1, 6 do
				if core.get_item_group(core.get_node(v(p.x, p.y + dy, p.z)).name, "water") == 0 then
					break
				end
				depth = dy
			end
			if depth >= 1 then spawn(name, {
				amount = math.ceil(2 * d),
				pos = {min = v(p.x - 0.4, p.y - 0.4, p.z - 0.4), max = v(p.x + 0.4, p.y, p.z + 0.4)},
				vel = {min = v(-0.05, 0.8, -0.05), max = v(0.05, 1.3, 0.05)},
				jitter = {min = v(-0.3, 0, -0.3), max = v(0.3, 0, 0.3)},
				exptime = depth / 1.3, -- pops just below the surface
				size = {min = 0.8, max = 1.8},
				texture = {name = "fx_ambience_bubble.png^[multiply:#e0f4ff", blend = "screen"},
			}) end
		end
	end
end

function feature.leaves(name, minp, maxp, d)
	local list = core.find_nodes_in_area(minp, maxp, {"group:leaves"})
	local n = 0
	for _, p in ipairs(pick(list, 16)) do
		if is_air(v(p.x, p.y - 1, p.z)) then
			local node = core.get_node(p)
			spawn(name, {
				amount = math.random() < 0.5 * d and 1 or 0,
				pos = {min = v(p.x - 0.4, p.y - 0.55, p.z - 0.4), max = v(p.x + 0.4, p.y - 0.55, p.z + 0.4)},
				vel = {min = v(-0.3, -0.5, -0.3), max = v(0.3, -0.3, 0.3)},
				acc = v(0, -0.1, 0),
				jitter = {min = v(-0.8, -0.1, -0.8), max = v(0.8, 0.1, 0.8)},
				exptime = 8,
				size = {min = 2.5, max = 4.0},
				collisiondetection = true,
				collision_removal = false,
				node = {name = node.name, param2 = node.param2},
			})
			n = n + 1
			if n >= 2 then break end
		end
	end
end

local ORDER = {"fireflies", "motes", "embers", "drips", "bubbles", "leaves"}

local timer = 0
core.register_globalstep(function(dtime)
	timer = timer + dtime
	if timer < INTERVAL then return end
	timer = 0
	if not enabled() then return end
	local d = density()
	if d <= 0 then return end
	for _, player in ipairs(core.get_connected_players()) do
		local pp = vector.round(player:get_pos())
		local minp = v(pp.x - R, pp.y - RY, pp.z - R)
		local maxp = v(pp.x + R, pp.y + RY, pp.z + R)
		local name = player:get_player_name()
		for _, f in ipairs(ORDER) do
			if S:get_bool("fx_particles_" .. f, true) then
				local before = spawned
				feature[f](name, minp, maxp, d, pp)
				if debug and spawned > before then
					core.log("action", ("[fx_ambience] %s: %d spawners"):format(f, spawned - before))
				end
			end
		end
	end
end)
