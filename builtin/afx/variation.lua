-- The ambience breathes. A place doesn't sound the same every minute: the
-- birds go quiet together for a while (and that silence is the eerie part),
-- then the place livens up; inside that, each bed wanders a little, the
-- balance between a place's beds shifts, the air's brightness drifts, beds
-- swap takes, and which birds are about rotates. Thunder or a blast stops
-- the birds at once. Everything moves slowly (tens of seconds) so it reads
-- as life, not as the mix fidgeting.
--
-- Tunables: the second table in levels.lua (`variation`), scaled live by the
-- mixer's Variation sliders; /afx_var sets any of them until /afx_reload.
--
-- Used by ambience.lua: V.step once a scan, V.apply on the loops it wants
-- (beds and the insects over flowers), V.thin / V.interval / V.species for
-- the one-shots.

local A = afx_ambience
local L = A.lib
local V = {}
A.var = V

local function T() return A.TUNING.variation end

-- The mixer's knobs: multipliers on the table (saved as settings)
V.KNOBS = {
	{"depth", "Silences: how deep", "How far the place goes quiet in a lull"},
	{"often", "Silences: how often", "How likely a lull is (dusk, night, wind and thunder make them likelier)"},
	{"pace", "Pace (slower is longer phases)", "How long a phase lasts and how slowly it changes"},
	{"wander", "Wander", "How much each sound drifts, and the balance between them"},
	{"tone", "Tone drift", "How much the air's brightness drifts"},
	{"blend", "Biome blend speed", "How fast one place's sound may turn into the next's"},
}
local knob = {}
local function read_knob(k)
	knob[k] = tonumber(core.settings:get("afx_ambience_var_" .. k) or "") or 1
end
for _, k in ipairs(V.KNOBS) do read_knob(k[1]) end
function V.knob(k) return knob[k] or 1 end
function V.set_knob(k, v)
	knob[k] = v
	core.settings:set("afx_ambience_var_" .. k, string.format("%.2f", v))
end
function V.enabled()
	return T().enabled and core.settings:get_bool("afx_ambience_variation") ~= false
end

local clamp = function(x, a, b) return math.max(a, math.min(b, x)) end
local function smooth(t) t = clamp(t, 0, 1) return t * t * (3 - 2 * t) end

--[[ Slow noise: one smooth random curve per channel, -1..1 ]]

local noise = {}
local function wander(name, period, dt)
	local n = noise[name]
	if not n then
		n = {a = L.rand(-1, 1), b = L.rand(-1, 1), t = L.rand(0, 1), len = period * L.rand(0.7, 1.3)}
		noise[name] = n
	end
	n.t = n.t + dt / n.len
	while n.t >= 1 do
		n.t = n.t - 1
		n.a, n.b = n.b, L.rand(-1, 1)
		n.len = period * L.rand(0.7, 1.3) -- never quite periodic
	end
	return n.a + (n.b - n.a) * smooth(n.t)
end
-- read a channel without moving it (it moves once a scan, in step)
local function peek(name)
	local n = noise[name]
	if not n then return 0 end
	return n.a + (n.b - n.a) * smooth(n.t)
end

--[[ Phases: lull, normal, lively ]]

local S = {
	phase = "normal", phase_end = 0,
	from = {bed_db = 0, rate = 1}, to = {bed_db = 0, rate = 1}, t0 = 0, len = 1,
	startle = 0, startled_at = -1e9,
	now = 0, cur = {bed_db = 0, rate = 1},
	takes = {}, roster = nil, roster_next = 0, universe = {},
}

-- the phase's values with the knobs applied
local function values(phase)
	local t, d = T(), knob.depth
	local v = t[phase] or t.normal
	if phase == "lull" then
		return {bed_db = v.bed_db * d, rate = v.rate ^ d}
	end
	return {bed_db = v.bed_db, rate = v.rate}
end

local function context_bias(ctx)
	local t = T()
	local b = 1
	if ctx.day < 0.5 then b = b * t.lull_night end
	if ctx.tod > 0.72 and ctx.tod < 0.8 then b = b * t.lull_dusk end
	if (ctx.st.wind or 0) > 1.2 and ctx.rain == 0 then b = b * t.lull_wind end
	if S.now - S.startled_at < 90 then b = b * t.lull_startled end
	return b
end

local function choose(ctx)
	local t = T()
	local w = t.weight
	local wl = w.lull * knob.often * context_bias(ctx)
	local total = wl + w.normal + w.lively
	local function draw()
		local r = L.rand(0, total)
		if r < wl then return "lull" end
		if r < wl + w.normal then return "normal" end
		return "lively"
	end
	local p = draw()
	if p == S.phase then p = draw() end -- prefer a change, don't force it
	return p
end

local function enter(ctx, phase)
	local t = T()
	S.from = {bed_db = S.cur.bed_db, rate = S.cur.rate}
	S.to = values(phase)
	S.t0 = S.now
	S.len = t.transition * knob.pace
	S.phase = phase
	S.phase_end = S.now + L.rand(t.phase_min, t.phase_max) * knob.pace
	L.log("breath: " .. phase)
end

--[[ Startle: thunder, a blast ]]

-- k 0..1: how loud it arrived
function V.startle(k)
	if not V.enabled() or k < 0.15 then return end
	S.startle = math.max(S.startle, clamp(k, 0, 1))
	S.startled_at = S.now
end

--[[ Once a scan ]]

function V.step(ctx, dt)
	S.now = ctx.now
	local t = T()
	if not V.enabled() then
		S.cur = {bed_db = 0, rate = 1}
		S.startle = 0
		return
	end
	if S.phase_end == 0 then
		-- start somewhere in the middle of a normal stretch
		S.phase_end = S.now + L.rand(t.phase_min, t.phase_max) * 0.5 * knob.pace
	elseif S.now >= S.phase_end then
		enter(ctx, choose(ctx))
	end
	-- the move between phases (rate in log space: x0.2 to x1 sounds even)
	local f = smooth((S.now - S.t0) / math.max(1, S.len))
	S.cur.bed_db = S.from.bed_db + (S.to.bed_db - S.from.bed_db) * f
	S.cur.rate = math.exp(math.log(S.from.rate) + (math.log(S.to.rate) - math.log(S.from.rate)) * f)
	-- a startle wears off
	S.startle = math.max(0, S.startle - dt / math.max(1, t.startle_recover))
	-- the curves move
	local pace = knob.pace
	for key in pairs(t.breathes) do
		wander("db:" .. key, t.drift_period * pace, dt)
		wander("bal:" .. key, t.balance_period * pace, dt)
	end
	for kind in pairs(t.rate_breathes) do wander("rate:" .. kind, t.drift_period * pace, dt) end
	wander("tone", t.tone_period * pace, dt)
	V.step_roster(ctx)
end

--[[ Loops: what V.apply does to `wanted` ]]

-- The beds' other takes (util/afx/import_sounds.py makes them)
local TAKES = {
	b_birds = {"afx_out_birds_far", "afx_out_birds_far_b"},
	b_crickets = {"afx_out_crickets", "afx_out_crickets_b"},
	b_trees = {"afx_out_wind_trees", "afx_out_wind_trees_b"},
	b_howl = {"afx_out_wind_howl", "afx_out_wind_howl_b"},
	b_jungle = {"afx_out_jungle", "afx_out_jungle_b", "afx_out_jungle_c"},
	b_frogs = {"afx_out_frogs", "afx_out_frogs_b"},
	b_waves = {"afx_out_waves", "afx_out_waves_b"},
}
local available = {}
local function has_take(name)
	if available[name] == nil then available[name] = core.afx_has_sound(name) end
	return available[name]
end

-- how much `key` breathes (exact name, or a prefix like "fl" for the
-- flower insects, whose keys go on with their cell)
local function breath_of(key)
	local b = T().breathes
	local c = b[key]
	if c then return c end
	for k, v in pairs(b) do
		if key:sub(1, #k) == k then return v end
	end
	return nil
end

local function take_for(ctx, key, w)
	local list = TAKES[key]
	if not list or w.sound ~= list[1] then return key end
	local t = T()
	local tk = S.takes[key]
	if not tk then
		tk = {i = math.random(#list), next = S.now + L.rand(t.takes_min, t.takes_max) * 0.5}
		S.takes[key] = tk
	end
	if S.now >= tk.next then
		local old = key .. "@" .. tk.i
		local i = tk.i
		for _ = 1, 4 do
			i = math.random(#list)
			if i ~= tk.i and has_take(list[i]) then break end
		end
		if i ~= tk.i and has_take(list[i]) then
			-- the playing take lets go slowly; the new one comes up as slowly
			local e = ctx.st.emitters[old]
			if e then e.fade_out = math.max(0.004, e.gain / t.takes_fade) end
			tk.i = i
			tk.swapped = S.now
		end
		tk.next = S.now + L.rand(t.takes_min, t.takes_max) * knob.pace
	end
	if not has_take(list[tk.i]) then tk.i = 1 end
	w.sound = list[tk.i]
	if tk.swapped and S.now - tk.swapped < t.takes_fade then
		w.fade = math.min(w.fade or 0.5, math.max(0.004, w.gain / t.takes_fade))
	end
	return key .. "@" .. tk.i
end

-- Scale the loops that breathe; called on everything a scan wants, before
-- the reconcile (gains here are before levels and layer volumes)
function V.apply(ctx, wanted)
	if not V.enabled() then return end
	local t = T()
	local wd = t.drift_db * knob.wander
	local bal = clamp(t.balance * knob.wander, 0, 0.9)
	local tone = 1 - clamp(t.tone * knob.tone, 0, 0.8) * (0.5 + 0.5 * peek("tone"))
	local startle_db = t.startle_db * knob.depth * S.startle
	local keys, sum, sumf = {}, 0, 0
	for key, w in pairs(wanted) do
		local c = breath_of(key)
		if c and (w.gain or 0) > 0 then
			local db = c * (S.cur.bed_db + startle_db) + wd * math.sqrt(c) * peek("db:" .. key)
			w.gain = w.gain * 10 ^ (db / 20)
			if t.breathes[key] then
				-- the beds of a place share it between them
				local f = 1 + bal * peek("bal:" .. key)
				keys[#keys + 1] = {key, f}
				sum = sum + w.gain ^ 2
				sumf = sumf + (w.gain * f) ^ 2
				w.tone = tone
			end
		end
	end
	local norm = sumf > 0 and math.sqrt(sum / sumf) or 1
	for _, kf in ipairs(keys) do
		local key, f = kf[1], kf[2]
		local w = wanted[key]
		w.gain = w.gain * f * norm
		local k2 = take_for(ctx, key, w)
		if k2 ~= key then
			wanted[key] = nil
			wanted[k2] = w
		end
	end
end

--[[ One-shots ]]

-- the rate of one-shots of a kind ("life": birds, frogs; "place": drips,
-- creaks), x normal
function V.rate(kind)
	if not V.enabled() then return 1 end
	local t = T()
	local c = t.rate_breathes[kind] or 0
	local r = S.cur.rate * (1 + t.drift_rate * knob.wander * peek("rate:" .. kind))
	r = r * (1 + (t.startle_rate - 1) * S.startle)
	return math.max(0.01, r ^ c)
end
-- the next one-shot's wait: shorter when lively
function V.interval(kind, seconds)
	return seconds / math.max(1, V.rate(kind))
end
-- does this one-shot happen: fewer when quiet (thinning keeps their
-- timing random, and works from the moment the rate drops)
function V.thin(kind)
	local r = V.rate(kind)
	return r >= 1 or L.rand(0, 1) < r
end

-- Which birds are about: `roster` of them at a time, one changing now and
-- then. `list` is the candidates (with repeats for the common ones); what's
-- returned keeps those repeats.
function V.species(list)
	if not V.enabled() then return list end
	for _, n in ipairs(list) do
		if not S.universe[n] then
			S.universe[n] = true
			S.roster = nil
		end
	end
	if not S.roster then V.step_roster(nil, true) end
	local out = {}
	for _, n in ipairs(list) do
		if S.roster[n] then out[#out + 1] = n end
	end
	return #out > 0 and out or list
end

function V.step_roster(_, rebuild)
	local t = T()
	if rebuild or not S.roster then
		local all = {}
		for n in pairs(S.universe) do all[#all + 1] = n end
		table.sort(all)
		S.roster = {}
		for _ = 1, math.min(t.roster, #all) do
			local n
			repeat n = all[math.random(#all)] until not S.roster[n]
			S.roster[n] = true
		end
		S.roster_next = S.now + L.rand(t.roster_min, t.roster_max)
		return
	end
	if S.now < S.roster_next then return end
	S.roster_next = S.now + L.rand(t.roster_min, t.roster_max) * knob.pace
	local in_, out = {}, {}
	for n in pairs(S.universe) do
		if S.roster[n] then in_[#in_ + 1] = n else out[#out + 1] = n end
	end
	if #in_ == 0 or #out == 0 then return end
	table.sort(in_) table.sort(out)
	S.roster[in_[math.random(#in_)]] = nil
	S.roster[out[math.random(#out)]] = true
end

--[[ The tuning view ]]

function V.debug_lines()
	if not V.enabled() then return {"breath: off"} end
	local f = smooth((S.now - S.t0) / math.max(1, S.len))
	local lines = {}
	lines[1] = ("breath: %s%s  beds %+.1f dB  one-shots x%.2f  next in %.0fs%s"):format(
		S.phase, f < 1 and (" (%d%%)"):format(math.floor(f * 100)) or "", S.cur.bed_db, V.rate("life"),
		math.max(0, S.phase_end - S.now), S.startle > 0.01 and ("  startled %.2f"):format(S.startle) or "")
	local r = {}
	for n in pairs(S.roster or {}) do r[#r + 1] = n:gsub("^afx_bird_", "") end
	table.sort(r)
	local tk = {}
	for key, v in pairs(S.takes) do tk[#tk + 1] = key:gsub("^b_", "") .. string.char(96 + v.i) end
	table.sort(tk)
	lines[2] = ("  tone %.2f  birds about: %s  takes: %s"):format(
		1 - clamp(T().tone * knob.tone, 0, 0.8) * (0.5 + 0.5 * peek("tone")),
		table.concat(r, " "), table.concat(tk, " "))
	return lines
end

--[[ /afx_var: any tunable, live (until /afx_reload) ]]

core.register_chatcommand("afx_var", {
	params = "[<key> <value>] | phase <lull|normal|lively>",
	description = "Ambience variation tunables (levels.lua): list, or set one live",
	func = function(_, param)
		local key, val = param:match("^(%S+)%s+(%S+)$")
		local root = A.TUNING
		if key == "phase" then
			if val ~= "lull" and val ~= "normal" and val ~= "lively" then
				return false, "phase: lull, normal or lively"
			end
			V.force(val)
			return true, "breath: " .. val .. " (for 10 minutes, or until /afx_var phase)"
		end
		if not key then
			local out = {}
			local function dump(prefix, t)
				local ks = {}
				for k in pairs(t) do ks[#ks + 1] = k end
				table.sort(ks)
				for _, k in ipairs(ks) do
					local v = t[k]
					if type(v) == "table" then dump(prefix .. k .. ".", v)
					else out[#out + 1] = prefix .. k .. "=" .. tostring(v) end
				end
			end
			dump("", root)
			return true, table.concat(out, "  ")
		end
		-- variation.lull.bed_db, or lull.bed_db for short
		local path = {}
		for part in key:gmatch("[^%.]+") do path[#path + 1] = part end
		local t = root
		if root[path[1]] == nil then t = root.variation end
		for i = 1, #path - 1 do
			t = type(t) == "table" and t[path[i]] or nil
		end
		local last = path[#path]
		if type(t) ~= "table" or t[last] == nil or type(t[last]) == "table" then
			return false, "no such tunable: " .. key
		end
		if type(t[last]) == "boolean" then
			t[last] = val == "true" or val == "1"
		else
			local n = tonumber(val)
			if not n then return false, "not a number: " .. val end
			t[last] = n
		end
		-- a new phase now, so the change is heard
		S.phase_end = math.min(S.phase_end, S.now)
		return true, key .. " = " .. tostring(t[last]) .. " (until /afx_reload; levels.lua keeps it)"
	end,
})

-- for tests: jump to a phase now ("lull", "normal", "lively")
function V.force(phase)
	S.now = core.get_us_time() / 1e6 -- (between scans)
	S.phase_end = S.now + 600
	S.from = {bed_db = S.cur.bed_db, rate = S.cur.rate}
	S.to = values(phase)
	S.t0 = S.now
	S.len = T().transition * knob.pace
	S.phase = phase
end
V.state = S
