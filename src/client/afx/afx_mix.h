// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Buses and the per-source mix policy. Pure: no OpenAL here.

#pragma once

#include "afx_types.h"
#include <string>
#include <string_view>
#include <vector>

namespace afx
{

/// How a sound sits in the environment.
enum class Bus : u8 {
	World,   // positional: occlusion, air absorption, room reverb, echo
	Local,   // non-positional, heard from where the player is (footsteps, digging)
	Outdoor, // outdoor ambience: muffled by enclosure, arrives through the opening
	Indoor,  // room tone / cave tone: lightly reverberant, never muffled
	Dry,     // UI, music: untouched
};

const char *busName(Bus b);

/// Send levels and filters for one source, applied through EFX filters so the
/// source's own AL_GAIN (and its fades) stay untouched.
struct Mix {
	float direct_gain = 1.f;
	float direct_hf = 1.f;
	float reverb_send = 0.f;
	float reverb_hf = 1.f;
	float echo_send = 0.f;
	float muffle_send = 0.f;  // through the "behind a wall" EQ
	bool spatial_bed = false; // outdoor bed: pan toward the opening
};

/// Tiny glob: `*` and `?`, case-sensitive.
bool globMatch(std::string_view pattern, std::string_view text);

/// Comma-separated globs, whitespace trimmed, empty entries dropped.
std::vector<std::string> splitGlobs(const std::string &list);

struct BusRules {
	std::vector<std::string> dry;
	std::vector<std::string> outdoor;
	std::vector<std::string> indoor;

	static BusRules fromParams(const Params &p);
};

/// Prefixes always win (`afx_out_`, `afx_in_`, `afx_dry_`), then the glob
/// lists. Positional sounds are World unless explicitly dry.
Bus classify(const std::string &name, bool positional, const BusRules &rules);

/// Per-play variation of short one-shots (footsteps, digging, drips...):
/// a pitch factor (pitch and speed move together, like a tape) and a gain
/// factor. `u1..u4` are uniform randoms in [0, 1); `amount` scales it
/// (afx_variation). Roughly normal, ~±4 % pitch and ~±1.5 dB at 1.0, hard
/// limited to ±7 % so it never sounds detuned.
struct Variation {
	float pitch = 1.f;
	float gain = 1.f;
};
Variation vary(float u1, float u2, float u3, float u4, float amount);

/// Sounds shorter than this (seconds) get varied; longer ones are left alone.
constexpr float VARY_MAX_SECONDS = 2.5f;

/// The policy. `occ` is the source's smoothed occlusion (World only).
Mix mixFor(Bus bus, const Acoustics &env, const Occlusion &occ, const Params &params);

} // namespace afx
