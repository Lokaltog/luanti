// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Mood: two slow scalars, tension and cozy, from what the player can sense:
// how dark it is, how far the open sky is, how big and hard the space is,
// night, and signs of home (beds, chests, furnaces, lamps). The engine plays
// mood layers from them (a drone that creeps in deep in the dark, a warm
// hearth at home) and other client systems can read them too: the fx
// renderer can key exposure or grading on the same numbers.
//
// Contrast is the point: dread creeps in over ~20 s, relief arrives in ~3 s.

#pragma once

#include "afx_types.h"
#include <algorithm>
#include <cmath>

namespace afx
{

struct Mood {
	float tension = 0.f; // 0 calm .. 1 dread
	float cozy = 0.f;    // 0 .. 1 home
};

struct MoodInputs {
	float light = 1.f;    // 0..1, light level where the ear is (day/night blended)
	float daylight = 1.f; // 0..1, how much sun the sky gives right now
	int homey = 0;        // signs of home within ~8 nodes
	Acoustics env;        // the probe's (smoothed) acoustics
};

inline Mood moodTarget(const MoodInputs &in)
{
	auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
	const Acoustics &a = in.env;
	const float dark = 1.f - clamp01(in.light);
	// Underground: no open path to the sky and nothing through the walls
	const float sky = clamp01(std::max(a.outdoor_gain, a.outdoor_wall * 4.f));
	const float underground = 1.f - sky;
	// A big, hard, ringing space is eerier than a burrow
	const float cavern = clamp01((a.decay - 1.5f) / 3.f) * a.enclosure;
	const float night_out = (1.f - clamp01(in.daylight)) * (1.f - a.enclosure);
	// Home: its signs, and a light on
	const float home = clamp01(in.homey / 6.f) * clamp01(in.light * 1.5f);

	Mood m;
	// Darkness under the open sky is moonlight; darkness inside is the dark
	const float shut_in = std::max(underground, a.enclosure);
	m.tension = clamp01(0.45f * dark * (0.4f + 0.6f * shut_in) + 0.35f * underground +
			0.2f * cavern + 0.25f * night_out - 0.9f * home);
	m.cozy = clamp01(home * (0.5f + 0.5f * a.enclosure));
	return m;
}

class MoodSmoother
{
public:
	const Mood &current() const { return m_cur; }

	void step(float dt, const Mood &target, float scale = 1.f)
	{
		auto glide = [&](float &cur, float tgt, float up, float down) {
			float tau = (tgt > cur ? up : down) * std::max(0.05f, scale);
			cur += (tgt - cur) * (1.f - std::exp(-dt / tau));
		};
		glide(m_cur.tension, target.tension, 20.f, 3.f);
		glide(m_cur.cozy, target.cozy, 4.f, 5.f);
	}

	void reset(const Mood &m) { m_cur = m; }

private:
	Mood m_cur;
};

} // namespace afx
