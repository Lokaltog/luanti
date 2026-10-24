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
	float neighbor_send = 0.f;// into the next room's reverb
	float hollow_send = 0.f;  // into the cavity under the floor (footsteps)
	float remote_send = 0.f;  // into the far space a source sits in (send 5)
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
/// `footstep`: the player's own steps (hollow floors boom).
Mix mixFor(Bus bus, const Acoustics &env, const Occlusion &occ, const Params &params,
		bool footstep = false);

/// How much longer a source's own space rings than the listener's room,
/// weighted by how wet it is: 0 = nothing to carry, 1 = a cathedral heard
/// from a closet.
float spaceContrast(const SourceSpace &s, const Acoustics &env);

/// Pick what the remote slot plays this recast: the source space with the
/// most contrast, weighted by how loud the source reaches the ear (`reach`,
/// 0..1). Call per candidate; keeps the best in `best`/`best_w`.
void considerRemote(const SourceSpace &s, float reach, v3f dir, const Acoustics &env,
		RemoteSpace &best, float &best_w);

/// A World source carries its own surroundings: the far cave's tail on a
/// zombie in it (through send 5, if its space is like the one the slot
/// plays), and water's dullness on something under the surface. Applied on
/// top of mixFor.
void carrySpace(Mix &m, const SourceSpace &s, const RemoteSpace &r, const Acoustics &env,
		const Occlusion &occ, const Params &params);

/// The build's own voice: every enclosed space has a faint presence (air,
/// distant settling, the room ringing with nothing), stronger in hard rooms
/// and caves, absent under the sky. Drives one looping noise source.
struct RoomTone {
	float gain = 0.f;   // AL_GAIN for the tone
	float pitch = 1.f;  // bigger spaces sound lower
	Mix mix;            // mostly through the room's own reverb
};
RoomTone roomTone(const Acoustics &env, const Params &params);

} // namespace afx
