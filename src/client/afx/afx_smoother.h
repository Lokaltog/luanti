// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Glides the applied acoustics toward the latest probe result. The probe runs
// at 2-3 Hz; this runs every sound-thread step (~60 Hz), so parameter changes
// never step audibly. Time constants are asymmetric where the ear expects it:
// stepping outside opens up fast, walking into a cave closes in slower.

#pragma once

#include "afx_types.h"
#include <cmath>

namespace afx
{

class Smoother
{
public:
	void reset(const Acoustics &a) { m_cur = m_target = a; m_valid = true; }

	void setTarget(const Acoustics &a)
	{
		if (!m_valid)
			reset(a);
		m_target = a;
	}

	bool valid() const { return m_valid; }
	const Acoustics &current() const { return m_cur; }
	const Acoustics &target() const { return m_target; }

	/// `scale` multiplies every time constant (afx_smoothing).
	void step(float dt, float scale)
	{
		if (!m_valid || dt <= 0.f)
			return;
		scale = std::max(0.01f, scale);
		auto glide = [&](float &cur, float tgt, float tau_up, float tau_down) {
			float tau = (tgt > cur ? tau_up : tau_down) * scale;
			cur += (tgt - cur) * (1.f - std::exp(-dt / tau));
		};
		auto glide_v = [&](v3f &cur, const v3f &tgt, float tau) {
			float k = 1.f - std::exp(-dt / (tau * scale));
			cur += (tgt - cur) * k;
		};
		// A direction: rotate toward the target, keep unit length. A zero
		// target (no opening) keeps the last direction; focus fades instead.
		auto glide_dir = [&](v3f &cur, const v3f &tgt, float tau) {
			if (tgt.getLengthSQ() < 1e-6f)
				return;
			if (cur.getLengthSQ() < 1e-6f) {
				cur = tgt;
				return;
			}
			glide_v(cur, tgt, tau);
			if (cur.getLengthSQ() < 1e-4f) // exactly opposite: nudge sideways
				cur = tgt;
			cur.normalize();
		};

		const Acoustics &t = m_target;
		Acoustics &c = m_cur;
		glide(c.enclosure, t.enclosure, 0.6f, 0.5f);
		glide(c.mean_free_path, t.mean_free_path, 0.8f, 0.8f);
		glide(c.absorption, t.absorption, 0.8f, 0.8f);
		glide(c.hf, t.hf, 0.8f, 0.8f);

		glide(c.wet, t.wet, 0.6f, 0.5f);
		glide(c.decay, t.decay, 0.8f, 0.6f);
		glide(c.decay_hf_ratio, t.decay_hf_ratio, 0.8f, 0.8f);
		glide(c.density, t.density, 0.8f, 0.8f);
		glide(c.diffusion, t.diffusion, 0.8f, 0.8f);
		glide(c.reflections_gain, t.reflections_gain, 0.5f, 0.5f);
		glide(c.reflections_delay, t.reflections_delay, 0.5f, 0.5f);
		glide(c.late_gain, t.late_gain, 0.6f, 0.6f);
		glide(c.late_delay, t.late_delay, 0.5f, 0.5f);
		glide_v(c.reflections_pan, t.reflections_pan, 0.5f);

		glide(c.echo, t.echo, 0.5f, 0.4f);
		glide(c.echo_delay, t.echo_delay, 0.3f, 0.3f);
		glide(c.echo_feedback, t.echo_feedback, 0.5f, 0.5f);
		glide(c.echo_damping, t.echo_damping, 0.5f, 0.5f);
		glide_dir(c.echo_dir, t.echo_dir, 0.4f);

		glide(c.outdoor_gain, t.outdoor_gain, 0.35f, 0.7f);
		glide(c.outdoor_hf, t.outdoor_hf, 0.35f, 0.7f);
		glide(c.outdoor_wall, t.outdoor_wall, 0.35f, 0.7f);
		glide(c.outdoor_path, t.outdoor_path, 0.5f, 0.5f);
		glide(c.outdoor_focus, t.outdoor_focus, 0.5f, 0.4f);
		glide_dir(c.outdoor_dir, t.outdoor_dir, 0.4f);

		c.underwater = t.underwater;
		c.in_lava = t.in_lava;
		glide(c.submerged, t.submerged, 0.08f, 0.2f);
	}

private:
	Acoustics m_cur, m_target;
	bool m_valid = false;
};

} // namespace afx
