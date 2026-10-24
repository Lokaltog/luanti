-- /afx: a live mixer for the environmental audio. The engine's afx_* settings
-- (reverb, echo, occlusion, the score...) and the ambience's per-layer
-- volumes, as sliders that apply as you drag them and are saved. It's the
-- client's own: it works on any server.
--
-- /afx_report: what the ambience found in the game you're playing, to
-- check that a game's names and groups were picked up.

local A = afx_ambience
local F = core.formspec_escape

-- Engine settings: {key, label, max, default}
local ENGINE = {
	{"afx_reverb", "Room reverb", 2, 1},
	{"afx_convolution", "Recorded spaces (real caves, halls, rooms)", 1, 0.6},
	{"afx_echo", "Echoes (cliffs, canyons)", 2, 1},
	{"afx_occlusion", "Walls muffle sounds", 2, 1},
	{"afx_outdoor_muffle", "Outdoors muffled indoors", 4, 1},
	{"afx_roomtone", "Room tone (the build's own voice)", 2, 1},
	{"afx_source_space", "Sounds carry their own space", 2, 1},
	{"afx_mood", "Mood layers (dread, hearth)", 2, 1},
	{"afx_score", "Mood score (music)", 2, 0.8},
	{"afx_variation", "Variety of short sounds", 2, 1},
	{"afx_doppler", "Doppler", 2, 0.5},
}

local function get(key, default)
	return tonumber(core.settings:get(key) or "") or default
end

local function slider(y, name, label, value, max, tip)
	local pct = math.floor(value * 100 + 0.5)
	return ("label[0.4,%f;%s]"):format(y + 0.25, F(label)) ..
		(tip and ("tooltip[%s;%s]"):format(name, F(tip)) or "") ..
		("scrollbaroptions[min=0;max=%d;smallstep=5;largestep=25]"):format(max * 100) ..
		("scrollbar[6.6,%f;3.9,0.45;horizontal;%s;%d]"):format(y, name, pct) ..
		("label[10.7,%f;%d%%]"):format(y + 0.25, pct)
end

local scroll_at = {} -- per player: where the list was scrolled to

local function formspec(name)
	local rows = {}
	local y = 1.8
	rows[#rows + 1] = ("checkbox[0.4,0.45;afx_enable;%s;%s]"):format(F("Environmental audio on"),
		tostring(core.settings:get_bool("afx_enable") ~= false))
	rows[#rows + 1] = ("checkbox[6.6,0.45;afx_hrtf;%s;%s]"):format(F("Headphones (3D)"),
		tostring(core.settings:get_bool("afx_hrtf") == true))
	rows[#rows + 1] = ("checkbox[9.4,0.45;afx_debug_view;%s;%s]"):format(F("Tuning view"),
		tostring(core.settings:get_bool("afx_debug_view") == true))
	rows[#rows + 1] = "label[0.4,1.3;" .. F("Engine (applies live)") .. "]"
	for _, e in ipairs(ENGINE) do
		rows[#rows + 1] = slider(y, "e_" .. e[1], e[2], get(e[1], e[4]), e[3])
		y = y + 0.6
	end
	y = y + 0.3
	rows[#rows + 1] = ("label[0.4,%f;%s]"):format(y, F("Ambience layers"))
	y = y + 0.5
	for _, l in ipairs(A.LAYERS) do
		rows[#rows + 1] = slider(y, "v_" .. l[1], l[2], A.volume(l[1]), 2, l[3])
		y = y + 0.6
	end
	y = y + 0.2
	rows[#rows + 1] = ("checkbox[0.4,%f;afx_ambience_follower;%s;%s]"):format(y,
		F("Something follows you in the deep dark"),
		tostring(core.settings:get_bool("afx_ambience_follower") == true))
	rows[#rows + 1] = ("checkbox[6.6,%f;afx_ambience_treasure;%s;%s]"):format(y,
		F("Treasure shimmer"), tostring(core.settings:get_bool("afx_ambience_treasure") == true))
	y = y + 0.8
	-- a window that fits small screens; the list scrolls inside it
	local view = 10.5
	local extra = math.max(0, y - view)
	return "formspec_version[6]size[12.6," .. (view + 1.1) .. "]" ..
		("scrollbaroptions[min=0;max=%d;smallstep=10;largestep=50]"):format(math.ceil(extra * 10)) ..
		("scrollbar[12,0.1;0.45,%f;vertical;afx_scroll;%d]"):format(view, scroll_at[name] or 0) ..
		"scroll_container[0,0;11.9," .. view .. ";afx_scroll;vertical;0.1]" .. table.concat(rows) ..
		"scroll_container_end[]" ..
		("button_exit[4.8,%f;3,0.7;close;%s]"):format(view + 0.25, F("Done"))
end

core.register_chatcommand("afx", {
	description = "Environmental audio mixer",
	func = function(name)
		core.show_formspec(name, "afx:mixer", formspec(name))
		return true
	end,
})

core.register_on_player_receive_fields(function(player, formname, fields)
	if formname ~= "afx:mixer" then return end
	local name = player:get_player_name()
	local changed = false
	for k, v in pairs(fields) do
		local ev = core.explode_scrollbar_event(v)
		if k == "afx_scroll" then
			if ev.type == "CHG" or ev.type == "VAL" then scroll_at[name] = ev.value end
		elseif ev.type == "CHG" or ev.type == "VAL" then
			if k:sub(1, 2) == "e_" then
				local key, val = k:sub(3), string.format("%.2f", ev.value / 100)
				if core.settings:get(key) ~= val then
					core.settings:set(key, val)
					changed = true
				end
			elseif k:sub(1, 2) == "v_" then
				if math.abs(A.volume(k:sub(3)) - ev.value / 100) > 0.001 then
					A.set_volume(k:sub(3), ev.value / 100)
					changed = true
				end
			end
		end
	end
	for _, key in ipairs({"afx_enable", "afx_hrtf", "afx_debug_view", "afx_ambience_follower",
			"afx_ambience_treasure"}) do
		if fields[key] then core.settings:set_bool(key, fields[key] == "true") changed = true end
	end
	if fields.quit or fields.close then
		core.settings:write()
		return
	end
	-- redraw, so the numbers next to the sliders follow them
	if changed then
		core.show_formspec(name, "afx:mixer", formspec(name))
	end
end)

--[[ /afx_view: the tuning view (rays, every sound with its mix) ]]

core.register_chatcommand("afx_view", {
	description = "Show what the environmental audio hears: probe rays, sounds and their mix",
	func = function()
		local on = not core.settings:get_bool("afx_debug_view")
		core.settings:set_bool("afx_debug_view", on)
		return true, "afx tuning view " .. (on and "on" or "off")
	end,
})

--[[ /afx_report ]]

A.report = A.report or {}

function A.add_report(fn) A.report[#A.report + 1] = fn end

core.register_chatcommand("afx_report", {
	description = "What the environmental audio found in this game (for tuning)",
	func = function(name)
		local player = core.get_player_by_name(name)
		local lines = {"afx_report " .. os.date("%Y-%m-%d %H:%M")}
		local function add(s) lines[#lines + 1] = s end
		local game = core.afx.game
		add("game: " .. tostring(game.id or "? (no afx_ambience bridge on the server)"))
		for _, m in ipairs({"mcl_weather", "lightning", "mcl_worlds", "mcl_bells", "mcl_fire",
				"fire", "vl_env_sounds", "env_sounds", "mcl_music", "vl_cavesounds", "mcl_mobs"}) do
			add(("mod %-14s %s"):format(m, core.get_modpath(m) and "yes" or "-"))
		end
		local w = {}
		for k, v in pairs(core.afx.world) do w[#w + 1] = k .. "=" .. tostring(v) end
		table.sort(w)
		add("server says: " .. table.concat(w, " "))
		local n = 0
		for _ in pairs(core.registered_entities) do n = n + 1 end
		add("creatures the server described: " .. n)
		if player then
			local h = player.get_acoustics and player:get_acoustics()
			if h then
				local ks = {}
				for k, val in pairs(h) do ks[#ks + 1] = ("%s=%.3g"):format(k, val) end
				table.sort(ks)
				add("engine hears: " .. table.concat(ks, " "))
			else
				add("engine hears: (no report: vanilla client or afx off)")
			end
			local ctx = {player = player, daylight = (core.get_natural_light or core.get_node_light)(
					vector.round(vector.offset(player:get_pos(), 0, 1.6, 0)), 0.5) or 0,
				under_sky = A.lib.sky_open(vector.round(vector.offset(player:get_pos(), 0, 1.6, 0)))}
			A.where(ctx)
			add(("so: under_sky=%s exposure=%.2f through=%.3f underground=%s indoors=%s"):format(
				tostring(ctx.under_sky), ctx.exposure, ctx.through, tostring(ctx.underground),
				tostring(ctx.indoors)))
			local pos = player:get_pos()
			local bd = core.get_biome_data(pos)
			add("biome here: " .. (bd and core.get_biome_name(bd.biome) or "?"))
			local below = core.get_node(vector.round(vector.offset(pos, 0, -0.6, 0))).name
			local def = core.registered_nodes[below] or {}
			local g = {}
			for k, v in pairs(def.groups or {}) do g[#g + 1] = k .. "=" .. v end
			table.sort(g)
			add("standing on: " .. below .. " {" .. table.concat(g, ",") .. "}")
			local fs = def.sounds and def.sounds.footstep
			add("its footstep: " .. tostring(type(fs) == "table" and fs.name or fs))
			local ents = {}
			for _, obj in ipairs(core.get_objects_inside_radius(pos, 32)) do
				local e = obj:get_luaentity()
				if e and e.name then
					ents[e.name] = (ents[e.name] or 0) + 1
				end
			end
			local el = {}
			for k, n in pairs(ents) do el[#el + 1] = k .. " x" .. n end
			table.sort(el)
			add("entities within 32: " .. (#el > 0 and table.concat(el, ", ") or "none"))
			for _, grp in ipairs({"group:leaves", "group:glass", "group:pane", "group:flower",
					"group:water", "group:lava", "group:wood", "group:stone"}) do
				local n = #core.find_nodes_in_area(vector.subtract(pos, 12), vector.add(pos, 12), {grp})
				add(("%-13s within 12: %d"):format(grp, n))
			end
		end
		for _, fn in ipairs(A.report) do
			for _, s in ipairs(fn()) do add(s) end
		end
		local text = table.concat(lines, "\n")
		core.log("action", "\n" .. text)
		core.show_formspec(name, "afx:report",
			"formspec_version[6]size[14,11]textarea[0.3,0.3;13.4,9.6;report;;" .. F(text) ..
			"]button_exit[5.5,10.1;3,0.7;ok;OK]")
		return true, "afx_report: also in the client log"
	end,
})
