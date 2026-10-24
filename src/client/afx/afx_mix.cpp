// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "afx_mix.h"

#include <algorithm>
#include <cmath>

namespace afx
{

const char *busName(Bus b)
{
	switch (b) {
	case Bus::World: return "world";
	case Bus::Local: return "local";
	case Bus::Outdoor: return "outdoor";
	case Bus::Indoor: return "indoor";
	case Bus::Dry: return "dry";
	}
	return "?";
}

bool globMatch(std::string_view pat, std::string_view s)
{
	size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
	while (t < s.size()) {
		if (p < pat.size() && (pat[p] == '?' || pat[p] == s[t])) {
			p++;
			t++;
		} else if (p < pat.size() && pat[p] == '*') {
			star = p++;
			mark = t;
		} else if (star != std::string_view::npos) {
			p = star + 1;
			t = ++mark;
		} else {
			return false;
		}
	}
	while (p < pat.size() && pat[p] == '*')
		p++;
	return p == pat.size();
}

std::vector<std::string> splitGlobs(const std::string &list)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= list.size()) {
		size_t end = list.find(',', start);
		if (end == std::string::npos)
			end = list.size();
		std::string item = list.substr(start, end - start);
		size_t a = item.find_first_not_of(" \t");
		size_t b = item.find_last_not_of(" \t");
		if (a != std::string::npos)
			out.push_back(item.substr(a, b - a + 1));
		start = end + 1;
	}
	return out;
}

BusRules BusRules::fromParams(const Params &p)
{
	return {splitGlobs(p.dry_sounds), splitGlobs(p.outdoor_sounds), splitGlobs(p.indoor_sounds)};
}

static bool anyMatch(const std::vector<std::string> &globs, const std::string &name)
{
	for (const auto &g : globs)
		if (globMatch(g, name))
			return true;
	return false;
}

Bus classify(const std::string &name, bool positional, const BusRules &rules)
{
	auto starts = [&](const char *prefix) { return name.rfind(prefix, 0) == 0; };
	if (starts("afx_dry_"))
		return Bus::Dry;
	if (positional)
		return Bus::World;
	if (starts("afx_out_"))
		return Bus::Outdoor;
	if (starts("afx_in_"))
		return Bus::Indoor;
	if (anyMatch(rules.dry, name))
		return Bus::Dry;
	if (anyMatch(rules.outdoor, name))
		return Bus::Outdoor;
	if (anyMatch(rules.indoor, name))
		return Bus::Indoor;
	return Bus::Local;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Mix mixFor(Bus bus, const Acoustics &env, const Occlusion &occ, const Params &params)
{
	Mix m;
	if (!params.enabled || bus == Bus::Dry)
		return m;

	switch (bus) {
	case Bus::World: {
		float o = std::clamp(occ.amount * params.occlusion, 0.f, 1.f);
		m.direct_gain = 1.f - 0.97f * o;
		m.direct_hf = std::pow(std::clamp(occ.hf, 0.f, 1.f), params.occlusion);
		// What makes it through the blocker, muffled like a neighbour's party
		m.muffle_send = o * std::clamp(occ.wall, 0.f, 1.f);
		// A source behind a wall excites less of the listener's room.
		m.reverb_send = 1.f - 0.6f * o;
		m.reverb_hf = m.direct_hf;
		m.echo_send = 1.f - o;
		break;
	}
	case Bus::Local:
		m.reverb_send = 1.f;
		m.echo_send = 1.f;
		break;
	case Bus::Outdoor:
		m.direct_gain = env.outdoor_gain;
		m.direct_hf = env.outdoor_hf;
		m.muffle_send = env.outdoor_wall;
		// Outdoor sound coming into a cave rings in it.
		m.reverb_send = (env.outdoor_gain + 0.5f * env.outdoor_wall) * env.enclosure * 0.7f;
		m.reverb_hf = env.outdoor_hf;
		m.spatial_bed = true;
		break;
	case Bus::Indoor:
		m.reverb_send = 0.4f;
		break;
	case Bus::Dry:
		break;
	}

	// Submerged: everything but the dry bus moves to the muffled path.
	if (env.submerged > 0.f) {
		const float s = env.submerged;
		m.muffle_send = lerpf(m.muffle_send, std::max(m.muffle_send, 0.7f * m.direct_gain), s);
		m.direct_gain *= 1.f - s;
		m.reverb_hf *= lerpf(1.f, 0.1f, s);
		m.echo_send *= 1.f - s;
	}
	return m;
}

} // namespace afx
