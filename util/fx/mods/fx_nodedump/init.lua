-- fx_nodedump: the game's own node definitions as JSON, so tools work from
-- what the game registers instead of guessing from Lua source or names.
--
-- Output: <world>/fx_nodes.json
--   { "format": 1, "game": "<gameid>", "nodes": { "<name>": { ... }, ... } }
-- Per node: drawtype, paramtype, paramtype2, light_source, sunlight_propagates,
-- walkable, liquidtype, use_texture_alpha, groups, tiles, overlay_tiles,
-- special_tiles (texture names; animation kept), post_effect_color, mod.
--
-- Set fx_nodedump_quit = true to shut the server down right after writing.

local function tiles(list)
	if type(list) ~= "table" then
		return nil
	end
	local out = {}
	for i, t in ipairs(list) do
		if type(t) == "string" then
			out[i] = {name = t}
		elseif type(t) == "table" then
			out[i] = {
				name = t.name or t.image,
				animation = t.animation,
				color = t.color,
				align_style = t.align_style,
				scale = t.scale,
				backface_culling = t.backface_culling,
			}
		end
	end
	return out
end

local function color(c)
	if type(c) == "table" then
		return {a = c.a, r = c.r, g = c.g, b = c.b}
	end
	return c
end

local function dump_nodes()
	local nodes = {}
	for name, def in pairs(core.registered_nodes) do
		nodes[name] = {
			mod = def.mod_origin,
			drawtype = def.drawtype or "normal",
			paramtype = def.paramtype,
			paramtype2 = def.paramtype2,
			light_source = def.light_source or 0,
			sunlight_propagates = def.sunlight_propagates or false,
			walkable = def.walkable ~= false,
			liquidtype = def.liquidtype or "none",
			use_texture_alpha = def.use_texture_alpha,
			groups = def.groups or {},
			tiles = tiles(def.tiles),
			overlay_tiles = tiles(def.overlay_tiles),
			special_tiles = tiles(def.special_tiles),
			post_effect_color = color(def.post_effect_color),
			description = def.description,
		}
	end
	local game = core.get_game_info and core.get_game_info().id or ""
	local json = core.write_json({format = 1, game = game, nodes = nodes}, true)
	local path = core.get_worldpath() .. "/fx_nodes.json"
	local ok = core.safe_file_write(path, json)
	core.log(ok and "action" or "error", "[fx_nodedump] " .. (ok and "wrote " or "could not write ")
		.. path)
end

core.register_on_mods_loaded(function()
	-- after every mod (and late overrides in on_mods_loaded) has run
	core.after(0, function()
		dump_nodes()
		if core.settings:get_bool("fx_nodedump_quit", false) then
			core.request_shutdown("fx_nodedump done", false, 0)
		end
	end)
end)
