-- What you do with your hands, heard the moment you do it (the client
-- knows before the server does): a blade or a tool swished through the air,
-- a soft rustle as you take something in hand. Close to the ear and quiet;
-- the room still rings them (they're on the local bus, not dry).
-- Items the game gives a swing sound of its own are left to it (the engine
-- doesn't call here for them).

local A = afx_ambience
local L = A.lib

-- swung, they move air: blades, axes, picks, the long ones
local function swishes(item)
	if item == "" then return nil end
	for _, w in ipairs({"sword", "axe", "pick", "trident", "mace", "spear", "scythe"}) do
		if item:find(w) then return 1.0 end -- (pickaxe matches "axe": fine, it's heavy too)
	end
	for _, w in ipairs({"shovel", "spade", "hoe", "stick", "rod", "bow"}) do
		if item:find(w) then return 0.6 end
	end
	return nil
end

local last_swing, last_wield = 0, nil

core.afx.register_on_action(function(what, pos, item)
	local now = core.get_us_time() / 1e6
	if what == "swing" or what == "swing_hit" then
		local k = swishes(item)
		if not k or now - last_swing < 0.22 then return end
		last_swing = now
		-- a heavier thing swishes lower
		A.play("afx_whizz", {gain = 0.05 * k, pitch = L.rand(0.5, 0.62) + 0.1 * (1 - k)}, true)
	elseif what == "wield" then
		if item == last_wield then return end
		local had = last_wield
		last_wield = item
		if had == nil or item == "" then return end -- the first look, an empty hand
		A.play("afx_cloth", {gain = 0.03, pitch = L.rand(1.05, 1.2)}, true)
	end
end)
