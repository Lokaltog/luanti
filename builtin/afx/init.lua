-- afx fork: the client's ambience runtime.
--
-- The engine does the acoustics (reverb, echo, occlusion, muffling: see
-- src/client/afx). These scripts decide *what* plays and *where*, from the
-- world the client has loaded: rain drumming on the roof above you, birds
-- in real leaves, lava bubbling where it is, drips under a lake. Every
-- sound is a real positional source, so the engine occludes and rings it
-- like anything else.
--
-- They run on the client, in their own Lua environment (not client-side
-- modding), so the ambience works on any server. The server's afx_ambience
-- mod, when there is one, only tells them what a client can't know: the
-- weather, the biome, the game's creatures, lightning and blasts.

local afxpath = core.get_builtin_path() .. "afx" .. DIR_DELIM
local commonpath = core.get_builtin_path() .. "common" .. DIR_DELIM

local shared = {}
dofile(afxpath .. "compat.lua")
dofile(commonpath .. "after.lua")
assert(loadfile(commonpath .. "item_s.lua"))(shared) -- push/read node

afx_ambience = {}
for _, f in ipairs({"ambience.lua", "variation.lua", "mixer.lua", "weather.lua", "water.lua", "life.lua",
		"places.lua", "body.lua", "cinema.lua", "discovery.lua", "creatures.lua", "landing.lua",
		"nature.lua", "hands.lua"}) do
	dofile(afxpath .. f)
end

core.afx_mods_loaded()

-- unset, as in client builtin
debug.getinfo = nil
