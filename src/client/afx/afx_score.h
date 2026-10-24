// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A generative score that follows the mood. Instead of random tracks, sparse
// phrases of real instrument samples (piano, bell, singing bowl, pitched to
// a scale) whose key, register and density follow tension, cozy and the
// time of day:
//
//   day     C major pentatonic, middle register, phrases of 3-4 notes
//   cozy    the same key, lower and slower, a bell now and then
//   night   A minor pentatonic, lower, sparser
//   dread   no melody: low bowl strikes, a semitone or a tritone apart
//   dawn    once a day, a rising phrase as the sun comes up
//
// It plays in sessions (a few minutes) with long rests between, so it stays
// special. Pure scheduling: the caller plays the notes it returns.

#pragma once

#include "afx_mood.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace afx
{

struct Note {
	const char *group; // sound group
	float pitch;       // playback rate (1 = the sample's own pitch)
	float gain;
};

class Score
{
public:
	/// `rand01` returns uniform [0, 1). `tod` is 0..1 (0.5 noon), `day` 0..1.
	template <typename R>
	void step(float dt, const Mood &mood, float tod, float day, R &&rand01,
			std::vector<Note> &out)
	{
		m_t += dt;
		// One rising phrase at dawn, whatever the session
		if (tod > 0.245f && tod < 0.27f) {
			if (!m_dawn_played) {
				m_dawn_played = true;
				queueDawn();
			}
		} else if (tod > 0.4f) {
			m_dawn_played = false;
		}

		// Sessions: play a few minutes, rest longer
		m_session -= dt;
		if (m_session <= 0.f) {
			m_playing = !m_playing;
			m_session = m_playing ? 120.f + 120.f * rand01() : 180.f + 240.f * rand01();
			m_next = 2.f;
		}

		// Release queued (timed) notes
		for (size_t i = 0; i < m_queue.size();) {
			m_queue[i].at -= dt;
			if (m_queue[i].at <= 0.f) {
				out.push_back(m_queue[i].note);
				m_queue[i] = m_queue.back();
				m_queue.pop_back();
			} else {
				i++;
			}
		}

		if (!m_playing)
			return;
		m_next -= dt;
		if (m_next > 0.f)
			return;

		if (mood.tension > 0.55f) {
			// Dread: no tune, just low strikes that don't resolve
			const float base = 0.5f;
			push(0.f, {"afx_dry_score_bowl", base, 0.55f});
			if (rand01() < 0.5f) {
				const float other = rand01() < 0.5f ? base * semis(1) : base * semis(6);
				push(2.5f + 2.f * rand01(), {"afx_dry_score_bowl", other, 0.4f});
			}
			m_next = 14.f + 20.f * rand01();
			return;
		}

		const bool night = day < 0.5f;
		const bool cozy = mood.cozy > 0.4f;
		// scale degrees in semitones from the root, root = C4 (the sample)
		static const int MAJOR[] = {0, 2, 4, 7, 9};
		static const int MINOR[] = {-3, 0, 2, 4, 7}; // A minor pentatonic: A C D E G
		const int *scale = night ? MINOR : MAJOR;
		const int octave_lo = night || cozy ? -12 : 0;

		const int len = night ? 2 + (int)(rand01() * 2) : 3 + (int)(rand01() * 2);
		float at = 0.f;
		for (int i = 0; i < len; i++) {
			// a gentle walk along the scale
			m_degree += (int)std::floor(rand01() * 5.f) - 2;
			if (m_degree < 0) m_degree = 1;
			if (m_degree > 9) m_degree = 8;
			const int semi = scale[m_degree % 5] + 12 * (m_degree / 5) + octave_lo;
			const float gain = (night ? 0.28f : 0.34f) * (0.8f + 0.4f * rand01());
			push(at, {"afx_dry_score_piano", semis(semi), gain});
			at += (night || cozy ? 1.4f : 0.9f) + 0.9f * rand01();
		}
		// a sustained root under some phrases; a bell's glint at home
		if (rand01() < 0.35f)
			push(0.f, {"afx_dry_score_bowl", semis(night ? -15 : -12), 0.3f});
		if (cozy && rand01() < 0.3f)
			push(at + 0.5f, {"afx_dry_score_bell", semis(scale[(int)(rand01() * 5)] + 12), 0.18f});
		m_next = at + (night ? 9.f : 5.f) + (night ? 10.f : 8.f) * rand01();
	}

	bool playing() const { return m_playing; }

	/// Start a session now (tests, debugging)
	void startSession(float seconds = 180.f) { m_playing = true; m_session = seconds; m_next = 0.f; }

private:
	struct Timed { float at; Note note; };
	std::vector<Timed> m_queue;
	float m_t = 0.f;
	float m_session = 20.f; // first session starts after a short while
	bool m_playing = false;
	float m_next = 0.f;
	int m_degree = 2;
	bool m_dawn_played = false;

	static float semis(int s) { return std::pow(2.f, s / 12.f); }
	void push(float at, Note n) { m_queue.push_back({at, n}); }

	void queueDawn()
	{
		static const int RISE[] = {-12, -8, -5, 0, 4, 7, 12};
		float at = 0.f;
		push(0.f, {"afx_dry_score_bowl", semis(-12), 0.35f});
		for (int s : RISE) {
			push(at, {"afx_dry_score_piano", semis(s), 0.3f});
			at += 0.75f;
		}
		push(at + 0.4f, {"afx_dry_score_bell", semis(12), 0.2f});
	}
};

} // namespace afx
