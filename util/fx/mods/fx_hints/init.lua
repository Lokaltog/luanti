-- fx_hints: what the fx renderer can't see on its own, sent to each player.
--
-- The client only knows its map and objects; the server knows the
-- dimension, the biome, the weather and lightning strikes. Every 0.5 s this
-- writes them into a hidden HUD text element named "fx_hints" (per player,
-- updated with hud_change only when it changes). The fx client parses it
-- and never draws it; other clients don't draw it either (it sits off
-- screen, fully transparent).
--
--   fx1;dim=<overworld|nether|end|void>;biome=<name>;weather=<none|rain|snow|thunder>;
--   flash=<count of nearby lightning strikes>;portal=<none|nether|end>
--
-- Game agnostic where possible: without Mineclonia the dimension is
-- "overworld", the biome comes from core.get_biome_data and the weather is
-- "none".

local INTERVAL = 0.5
local FLASH_RANGE = 160 -- nodes: strikes further away are not seen

local huds = {}     -- player name -> hud id
local last = {}     -- player name -> last text
local flashes = {}  -- player name -> strike counter

local function dimension(pos)
	if core.get_modpath("mcl_worlds") and mcl_worlds.pos_to_dimension then
		return mcl_worlds.pos_to_dimension(pos) or "overworld"
	end
	return "overworld"
end

local function biome(pos)
	if core.get_modpath("mcl_biome_dispatch") and mcl_biome_dispatch.get_biome_name then
		local ok, name = pcall(mcl_biome_dispatch.get_biome_name, pos)
		if ok and name then
			return name
		end
	end
	local data = core.get_biome_data(pos)
	return data and core.get_biome_name(data.biome) or ""
end

local function weather(pos)
	if not (core.get_modpath("mcl_weather") and mcl_weather.state) then
		return "none"
	end
	local state = mcl_weather.state
	if state == "none" then
		return "none"
	end
	if core.get_modpath("mcl_worlds") and mcl_worlds.has_weather and not mcl_worlds.has_weather(pos) then
		return "none"
	end
	-- the same rule Mineclonia uses for its own sky: snow where it's cold
	if mcl_weather.has_snow and mcl_weather.has_snow(pos) then
		return state == "thunder" and "thunder" or "snow"
	end
	if mcl_weather.has_rain and not mcl_weather.has_rain(pos) then
		return "none" -- desert: no rain here
	end
	return state
end

local function portal(pos)
	for _, dy in ipairs({0, 1}) do
		local name = core.get_node({x = pos.x, y = pos.y + dy, z = pos.z}).name
		if name == "mcl_portals:portal" or name:find("portal_nether") then
			return "nether"
		elseif name == "mcl_portals:portal_end" or name == "mcl_portals:portal_gateway" then
			return "end"
		end
	end
	return "none"
end

local function update(player)
	local name = player:get_player_name()
	local pos = vector.round(player:get_pos())
	local text = ("fx1;dim=%s;biome=%s;weather=%s;flash=%d;portal=%s"):format(
		dimension(pos), biome(pos), weather(pos), flashes[name] or 0, portal(pos))
	if text == last[name] then
		return
	end
	last[name] = text
	if huds[name] then
		player:hud_change(huds[name], "text", text)
	else
		huds[name] = player:hud_add({
			type = "text",
			name = "fx_hints",
			position = {x = -10, y = -10}, -- off screen
			text = text,
			number = 0x000000,
			alignment = {x = 1, y = 1},
			z_index = -1000,
		})
	end
end

if core.get_modpath("mcl_lightning") and mcl_lightning.register_on_strike then
	mcl_lightning.register_on_strike(function(pos)
		if not pos then
			return
		end
		for _, player in ipairs(core.get_connected_players()) do
			if vector.distance(player:get_pos(), pos) <= FLASH_RANGE then
				local name = player:get_player_name()
				flashes[name] = (flashes[name] or 0) + 1
				update(player) -- now, not at the next tick
			end
		end
	end)
end

core.register_on_leaveplayer(function(player)
	local name = player:get_player_name()
	huds[name], last[name], flashes[name] = nil, nil, nil
end)

local timer = 0
core.register_globalstep(function(dtime)
	timer = timer + dtime
	if timer < INTERVAL then
		return
	end
	timer = 0
	for _, player in ipairs(core.get_connected_players()) do
		update(player)
	end
end)
