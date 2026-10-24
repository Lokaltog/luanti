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
	if (starts("afx_in_") || starts("afx_mood_"))
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

Variation vary(float u1, float u2, float u3, float u4, float amount)
{
	Variation v;
	if (amount <= 0.f)
		return v;
	// sum of two uniforms: a triangle distribution, mostly small changes
	const float p = (u1 + u2 - 1.f) * 0.07f * amount;      // -0.07..0.07
	const float g = (u3 + u4 - 1.f) * 3.0f * amount;       // -3..3 dB
	v.pitch = std::exp(std::clamp(p, -0.07f, 0.07f));
	v.gain = std::pow(10.f, std::clamp(g, -3.f, 3.f) / 20.f);
	return v;
}

/// OpenAL's low-pass is only a shelf above 5 kHz, which leaves birdsong and
/// rain bright however low gainhf goes. So a dull path is split: the bright
/// share stays direct, the rest goes through the steep muffle EQ.
static void dullen(Mix &m, float gain, float hf)
{
	hf = std::clamp(hf, 0.f, 1.f);
	gain = std::clamp(gain, 0.f, 1.f);
	m.direct_gain = gain * (0.1f + 0.9f * hf);
	m.direct_hf = hf;
	// what the bright share leaves out, through the EQ (unity in the lows)
	m.muffle_send = std::min(1.f, m.muffle_send + (gain - m.direct_gain));
}

Mix mixFor(Bus bus, const Acoustics &env, const Occlusion &occ, const Params &params,
		bool footstep)
{
	Mix m;
	if (!params.enabled || bus == Bus::Dry)
		return m;

	switch (bus) {
	case Bus::World: {
		float o = std::clamp(occ.amount * params.occlusion, 0.f, 1.f);
		// What makes it through the blocker, muffled like a neighbour's party.
		// Nothing else does: rock between you and a bird is silence.
		const float through = std::clamp(occ.wall, 0.f, 1.f);
		float clear = 1.f - o;
		float hf = std::pow(std::clamp(occ.hf, 0.f, 1.f), params.occlusion);
		m.muffle_send = o * through;
		// Around the corner: heard from the opening (the engine moves the
		// source there), duller for the bend
		const float p = std::clamp(occ.path, 0.f, 1.f);
		if (p > 0.f) {
			clear = lerpf(clear, occ.path_gain, p);
			hf = lerpf(hf, occ.path_hf, p);
			m.muffle_send *= 1.f - 0.6f * p;
		}
		// The listener's room only rings with what reaches it: what comes
		// round or through the opening, and what comes through the wall
		const float wall_part = m.muffle_send;
		dullen(m, clear, hf);
		m.reverb_send = std::min(1.f, clear + wall_part);
		m.reverb_hf = hf;
		m.echo_send = clear;
		break;
	}
	case Bus::Local:
		m.reverb_send = 1.f;
		m.echo_send = 1.f;
		if (footstep && env.hollow > 0.f) {
			// boards over empty space: the cavity under them booms
			m.hollow_send = env.hollow;
		}
		break;
	case Bus::Outdoor:
		m.muffle_send = env.outdoor_wall;
		dullen(m, env.outdoor_gain, env.outdoor_hf);
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

	// Sound made here leaks into the space next door and its reverb comes
	// back through the opening
	if (bus == Bus::World || bus == Bus::Local)
		m.neighbor_send = std::clamp(env.coupling, 0.f, 1.f);

	// A loud moment: the ambience steps back for a second
	if (env.duck > 0.f && (bus == Bus::Outdoor || bus == Bus::Indoor)) {
		const float k = 1.f - 0.6f * std::clamp(env.duck, 0.f, 1.f);
		m.direct_gain *= k;
		m.muffle_send *= k;
		m.reverb_send *= k;
	}
	// A blast close by: the world goes dull and far away while your ears ring
	if (env.deafen > 0.f) {
		const float k = std::clamp(env.deafen, 0.f, 1.f);
		m.direct_hf *= 1.f - 0.93f * k;
		m.direct_gain *= 1.f - 0.5f * k;
		m.reverb_send *= 1.f - 0.6f * k;
		m.reverb_hf *= 1.f - 0.9f * k;
		m.echo_send *= 1.f - k;
		m.neighbor_send *= 1.f - k;
		m.hollow_send *= 1.f - k;
	}

	// Submerged: everything but the dry bus moves to the muffled path.
	if (env.submerged > 0.f) {
		// just under the surface, the world above still comes through: duller,
		// but there; a few nodes down it's gone
		float s = env.submerged;
		if ((bus == Bus::Outdoor || bus == Bus::World) && env.underwater) {
			s *= std::clamp(0.45f + 0.35f * (env.water_depth - 1.f), 0.45f, 1.f);
			// what comes through the surface is dull
			m.direct_hf *= lerpf(1.f, 0.25f, env.submerged);
		}
		m.muffle_send = lerpf(m.muffle_send, std::max(m.muffle_send, 0.7f * m.direct_gain), s);
		m.direct_gain *= 1.f - s;
		m.reverb_hf *= lerpf(1.f, 0.1f, s);
		m.echo_send *= 1.f - s;
	}
	return m;
}

static float smoothstep(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

float spaceContrast(const SourceSpace &s, const Acoustics &env)
{
	if (!s.valid)
		return 0.f;
	// How much a space rings: its reverb level, for tails of a second and
	// more. Outdoors has a long faint tail that colours nothing; inside, the
	// listener's own room already carries what is like it.
	const float there = std::clamp(s.wet * 1.6f, 0.f, 1.f) * std::clamp(s.decay / 1.5f, 0.f, 1.f);
	const float here = std::clamp(env.wet, 0.f, 1.f) * std::clamp(env.enclosure, 0.f, 1.f) *
			std::clamp(env.decay / 1.5f, 0.f, 1.f);
	return std::clamp((there - here) / 0.5f, 0.f, 1.f);
}

void considerRemote(const SourceSpace &s, float reach, v3f dir, const Acoustics &env,
		RemoteSpace &best, float &best_w)
{
	const float w = spaceContrast(s, env) * std::clamp(reach, 0.f, 1.f);
	if (w <= best_w || w < 0.02f)
		return;
	best_w = w;
	// big caverns read as only half enclosed from inside; their tails are long
	best.level = std::clamp(s.wet * 1.6f, 0.f, 1.f);
	best.decay = s.decay;
	best.hf_ratio = s.hf_ratio;
	best.dir = dir;
}

void carrySpace(Mix &m, const SourceSpace &s, const RemoteSpace &r, const Acoustics &env,
		const Occlusion &occ, const Params &params)
{
	if (!s.valid || params.source_space <= 0.f || !params.enabled)
		return;
	const float k = std::min(1.f, params.source_space);
	const float dry = 1.f - std::clamp(env.submerged, 0.f, 1.f);
	// Something under the surface while you're in the air: dull and close
	// to the water, its splashes thud.
	if (s.in_water) {
		const float w = k * dry;
		m.direct_hf *= lerpf(1.f, 0.15f, w);
		m.direct_gain *= lerpf(1.f, 0.6f, w);
		m.reverb_hf *= lerpf(1.f, 0.3f, w);
		m.echo_send *= 1.f - 0.7f * w;
	}
	if (r.level <= 0.f)
		return;
	// The slot plays one space; sources in a space like it feed it
	const float ratio = std::max(s.decay, 0.05f) / std::max(r.decay, 0.05f);
	const float similar = std::exp(-2.f * std::fabs(std::log(ratio)));
	const float o = std::clamp(occ.amount * params.occlusion, 0.f, 1.f) * (1.f - occ.path);
	// only what actually reaches us: behind solid rock, nothing of its
	// space either
	m.remote_send = params.source_space * spaceContrast(s, env) * similar *
			(1.f - o) * dry * (1.f - std::clamp(env.deafen, 0.f, 1.f));
	// what the space next door's slot already carries back through the
	// opening, the remote slot shouldn't play again
	m.remote_send *= 1.f - std::clamp(env.coupling, 0.f, 1.f);
	m.remote_send = std::clamp(m.remote_send, 0.f, 1.f);
}

RoomTone roomTone(const Acoustics &env, const Params &params)
{
	RoomTone t;
	if (!params.enabled || params.roomtone <= 0.f)
		return t;
	const float enclosed = smoothstep(0.55f, 0.95f, env.enclosure);
	const float hard = std::clamp(1.f - env.absorption, 0.f, 1.f);
	const float size = std::clamp(env.mean_free_path / 14.f, 0.f, 1.f);
	t.gain = params.roomtone * 0.06f * enclosed * (0.1f + 0.9f * hard * hard) * (0.55f + 0.45f * size);
	t.gain *= 1.f - std::clamp(env.submerged, 0.f, 1.f);
	t.gain *= 1.f - 0.7f * std::clamp(env.duck, 0.f, 1.f);
	t.gain *= 1.f - std::clamp(env.deafen, 0.f, 1.f);
	// a cavern is a low rumble, a cupboard a faint hiss of air
	t.pitch = std::clamp(1.25f - 0.55f * size, 0.6f, 1.25f);
	// barely any of it direct: it is the room ringing, so through the room
	t.mix.direct_gain = 0.3f;
	t.mix.direct_hf = 0.08f + 0.25f * std::clamp(env.hf, 0.f, 1.f);
	t.mix.reverb_send = 1.f;
	t.mix.reverb_hf = 0.3f + 0.5f * std::clamp(env.hf, 0.f, 1.f);
	return t;
}

void carryWeather(Mix &m, v3f to_source, const Params &p)
{
	const float dist = to_source.getLength();
	const float far = std::clamp((dist - 8.f) / 40.f, 0.f, 1.f);
	const float night = std::clamp(p.night, 0.f, 1.f);
	const float rain = std::clamp(p.rain, 0.f, 1.f);
	float absorb = 3.f * (1.f - 0.4f * night) * (1.f + 1.5f * rain);
	if (far > 0.f && p.wind > 0.f && p.wind_dir.getLengthSQ() > 0.5f) {
		v3f travel = to_source * (-1.f / std::max(dist, 1e-3f));
		travel.Y = 0.f;
		const float c = travel.getLengthSQ() > 1e-4f ?
				travel.normalize().dotProduct(p.wind_dir) : 0.f;
		const float w = std::clamp(p.wind / 1.4f, 0.f, 1.f);
		const float shadow = std::clamp(-c, 0.f, 1.f) * w * far;
		m.direct_gain *= 1.f - 0.65f * shadow;
		m.direct_hf *= 1.f - 0.6f * shadow;
		m.reverb_send *= 1.f - 0.3f * shadow;
		// with the wind, sound bends back down and carries
		absorb *= 1.f - 0.45f * std::clamp(c, 0.f, 1.f) * w;
	}
	// rain hiss over the distance
	m.direct_gain *= 1.f - 0.3f * rain * far;
	m.air_absorption = std::clamp(absorb, 0.5f, 10.f);
}

} // namespace afx
