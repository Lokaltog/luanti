-- A soft chime the first time you find a new kind of place: a biome, a
-- cavern, the sky, another world. Once ever, per world.

local A = afx_ambience

-- C major pentatonic, as the note stones (the server's afx_ambience)
local SCALE = {0, 2, 4, 7, 9, 12, 14, 16}
local function semis(s) return 2 ^ (s / 12) end

--[[ First discoveries: a soft rising chime, once ever per kind of place ]]

local seen_cache = {}
local function seen(player)
	local name = player:get_player_name()
	if not seen_cache[name] then
		local s = player:get_meta():get_string("afx_seen")
		seen_cache[name] = s ~= "" and core.deserialize(s) or {}
	end
	return seen_cache[name]
end
core.register_on_leaveplayer(function(p) seen_cache[p:get_player_name()] = nil end)

local function remember(player, t)
	seen_cache[player:get_player_name()] = t
	player:get_meta():set_string("afx_seen", core.serialize(t))
end

local function chime(pname, big)
	local degs = big and {1, 3, 4, 6} or {3, 4, 6}
	for n, deg in ipairs(degs) do
		core.after((n - 1) * 0.32, function()
			A.play("afx_dry_score_bell", {to_player = pname, gain = 0.22,
				pitch = semis(SCALE[deg] + 12)}, true)
		end)
	end
	core.after(#degs * 0.32, function()
		A.play("afx_dry_score_bowl", {to_player = pname, gain = 0.2, pitch = semis(12)}, true)
	end)
end

-- What kind of place: a few broad kinds rather than every biome variant
local function kinds(ctx)
	local out = {}
	if ctx.dimension ~= "overworld" then
		out[#out + 1] = ctx.dimension
		return out
	end
	local bd = core.get_biome_data(ctx.ppos)
	local b = bd and core.get_biome_name(bd.biome) or ""
	b = b:gsub("_deep_ocean$", ""):gsub("_ocean$", ""):gsub("_underground$", "")
		:gsub("_beach$", ""):gsub("_shore$", "")
	if ctx.under_sky and b ~= "" then out[#out + 1] = "biome:" .. b:lower() end
	local h = ctx.hearing
	if ctx.underground and h and (h.decay or 0) > 1.6 and (h.mfp or 0) > 6 then
		out[#out + 1] = "cavern"
	end
	if ctx.under_sky and ctx.ppos.y > 60 then out[#out + 1] = "sky" end
	if ctx.underwater then out[#out + 1] = "underwater" end
	return out
end

A.register_layer(function(ctx)
	-- what's been found is kept per world: wait to know which (a server
	-- without the bridge never says: then it's per server)
	if not core.afx.game.id and core.get_gametime() < 15 then return end
	local st = ctx.st
	local now = ctx.now
	if now < (st.next_discovery or 0) then return end
	if (ctx.hearing and (ctx.hearing.tension or 0) > 0.6) then return end -- not mid-fright
	local list = kinds(ctx)
	if #list == 0 then return end
	-- stay a moment before it counts
	st.dwell = st.dwell or {}
	local mem
	for _, k in ipairs(list) do
		st.dwell[k] = (st.dwell[k] or 0) + 1
		if st.dwell[k] >= 4 then
			mem = mem or seen(ctx.player)
			if not mem[k] then
				mem[k] = os.time()
				remember(ctx.player, mem)
				st.next_discovery = now + 20
				core.log("action", "afx_ambience: " .. ctx.pname .. " discovered " .. k)
				chime(ctx.pname, k == "cavern" or k == "sky" or not k:find("^biome:"))
				break
			end
		end
	end
	for k in pairs(st.dwell) do
		local here = false
		for _, l in ipairs(list) do if l == k then here = true end end
		if not here then st.dwell[k] = nil end
	end
end)

