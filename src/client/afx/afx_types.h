// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared types between the main thread (which copies the map around the
// listener into a VoxelSnapshot) and the sound thread (which analyses it and
// drives OpenAL EFX). Nothing here touches OpenAL or the client, so the
// analysis is unit-testable on synthetic worlds.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace afx
{

/// Acoustic material of a voxel. Kept to a nibble (see VoxelSnapshot::cells).
enum class Material : u8 {
	Air = 0,     // passable: air, plants, torches, rails, flowers
	Leaky,       // partial obstruction: fences, panes, doors, slabs, ladders
	Foliage,     // leaves
	Soft,        // wool, carpet, beds, hay
	Snow,
	Glass,
	Wood,        // planks, logs, wooden blocks
	Earth,       // dirt, sand, gravel, clay (porous)
	Stone,       // stone, ores, bricks, metal, deepslate
	Water,
	Lava,
	Count
};

constexpr int MATERIAL_COUNT = static_cast<int>(Material::Count);

/// Per-material acoustic constants. Absorption is the Sabine coefficient
/// (broadband, roughly 500 Hz-2 kHz), `hf` how much of the high band survives
/// a reflection relative to the low band. The transmission costs drive the
/// outdoor-path search: `pass` is the cost (in equivalent nodes of air) of
/// sound crossing one voxel, `pass_hf` the extra high-frequency loss in nepers.
struct MaterialProps {
	const char *name;
	float absorption;
	float hf;
	float pass;
	float pass_hf;
};

const MaterialProps &props(Material m);

/// Bits packed next to the material nibble.
enum CellFlags : u8 {
	CELL_SKY = 1 << 4,     // air with full daylight: open to the sky
	CELL_UNLOADED = 1 << 5,// map not loaded here; treated as open air, not sky
	// Bits 6-7: thin nodes (doors, panes, trapdoors). 0 = full voxel, else
	// 1/2/3 = blocks only travel along X/Y/Z. A closed door blocks the
	// doorway; the same door swung open blocks nothing on the way through.
	CELL_THIN_SHIFT = 6,
	CELL_THIN_MASK = 3 << 6,
};

inline Material cellMaterial(u8 cell) { return static_cast<Material>(cell & 0x0f); }
inline bool cellSky(u8 cell) { return cell & CELL_SKY; }
/// -1 for a full voxel, else the axis (0 x, 1 y, 2 z) the thin node blocks.
inline int cellThinAxis(u8 cell) { return ((cell & CELL_THIN_MASK) >> CELL_THIN_SHIFT) - 1; }
inline u8 makeThin(Material m, int axis) { return (u8)m | (u8)((axis + 1) << CELL_THIN_SHIFT); }

/// Does this voxel stop sound travelling along `axis`?
inline bool cellBlocks(u8 cell, int axis)
{
	if (cellMaterial(cell) == Material::Air)
		return false;
	int t = cellThinAxis(cell);
	return t < 0 || t == axis;
}

/// Materials that sound passes through as "air" (with a cost) rather than
/// being transmitted through a solid wall: leaves, fences, water.
inline bool materialPorous(Material m)
{
	return m == Material::Air || m == Material::Leaky || m == Material::Foliage ||
			m == Material::Water;
}

/// A cube of voxels around the listener, copied on the main thread.
/// Immutable once published; shared with the sound thread by shared_ptr.
struct VoxelSnapshot {
	v3s16 origin;          // node position of cells[0]
	s16 size = 0;          // edge length in nodes
	std::vector<u8> cells; // size³, x fastest, then y, then z

	bool contains(v3s16 p) const {
		return p.X >= origin.X && p.Y >= origin.Y && p.Z >= origin.Z &&
				p.X < origin.X + size && p.Y < origin.Y + size && p.Z < origin.Z + size;
	}
	u32 index(v3s16 p) const {
		return (u32)(p.X - origin.X) + (u32)size * ((u32)(p.Y - origin.Y) +
				(u32)size * (u32)(p.Z - origin.Z));
	}
	/// Out-of-range positions read as unloaded air.
	u8 at(v3s16 p) const { return contains(p) ? cells[index(p)] : (u8)CELL_UNLOADED; }
};

/// Tuning, read from afx_* settings on the main thread and shipped with each
/// snapshot so the sound thread never touches g_settings.
struct Params {
	bool enabled = true;         // afx_enable
	float probe_interval = 0.4f; // afx_probe_interval, seconds
	s16 probe_radius = 32;       // afx_probe_radius, nodes (snapshot is 2r³)
	int rays = 64;               // afx_rays
	float reverb = 1.0f;         // afx_reverb, wet multiplier
	float max_decay = 5.0f;      // afx_max_decay, seconds
	float echo = 1.0f;           // afx_echo
	float occlusion = 1.0f;      // afx_occlusion
	float outdoor_muffle = 1.0f; // afx_outdoor_muffle
	float smoothing = 1.0f;      // afx_smoothing, scales all time constants
	float doppler = 0.5f;        // afx_doppler, OpenAL doppler factor
	float mood = 1.0f;           // afx_mood, level of the tension/cozy layers
	float variation = 1.0f;      // afx_variation, pitch/level variety of short sounds
	float score = 0.8f;          // afx_score, level of the mood-driven score (0 = off)
	bool score_replaces_music = true; // afx_score_replaces_music
	float roomtone = 1.0f;       // afx_roomtone, the room's own faint presence
	float source_space = 1.0f;   // afx_source_space, sounds carry their own surroundings
	bool hrtf = false;           // afx_hrtf, 3D sound for headphones
	// Weather, from the server mod (not settings)
	v3f wind_dir;                // node space, horizontal unit (toward where it blows)
	float wind = 0.f;            // 0 calm .. ~1.6 gale
	float rain = 0.f;            // 0..1
	float night = 0.f;           // 0..1 (from the client's time of day)
	bool debug = false;          // afx_debug
	// Bus routing by sound name, comma-separated globs (see afx_mix.h)
	std::string dry_sounds =     // afx_dry_sounds
		"*music*,*_ui_*,*gui*,*btn*,*button*,*click*,*inventory*,*award*,*achievement*";
	std::string outdoor_sounds = // afx_outdoor_sounds
		"*rain*,*wind*,*weather*,*thunder*";
	std::string indoor_sounds;   // afx_indoor_sounds
};

/// What the probe concluded about the listener's surroundings. These are the
/// *targets*; the sound thread smooths toward them between probes.
struct Acoustics {
	// Room
	float enclosure = 0.f;     // 0 open sky .. 1 sealed
	float mean_free_path = 0.f;// nodes, average ray length (open rays count max)
	float absorption = 1.f;    // area-weighted average absorption (open = 1)
	float hf = 1.f;            // average high-frequency reflectivity of hits

	// Reverb (EAX semantics)
	float wet = 0.f;           // overall reverb send, 0..1
	float decay = 0.3f;        // RT60, seconds
	float decay_hf_ratio = 1.f;// 0.1..2
	float density = 1.f;       // 0..1, small rooms are dense
	float diffusion = 1.f;     // 0..1, rough walls diffuse
	float reflections_gain = 0.05f;
	float reflections_delay = 0.007f; // seconds
	float late_gain = 1.f;
	float late_delay = 0.011f; // seconds
	v3f reflections_pan;       // node space, |v| = focus 0..1

	// Discrete echo off a far hard surface
	float echo = 0.f;          // strength 0..1
	float echo_delay = 0.1f;   // seconds, first tap
	float echo_feedback = 0.f; // 0..1, flutter in parallel walls
	float echo_damping = 0.5f; // 0..0.99
	v3f echo_dir;              // node space, unit or zero

	// Outdoors reaching the listener, by two paths. The open path goes
	// through air (doorways, cave mouths, leaves) and stays airy; the wall
	// path is transmitted through solids and gets the steep "behind a wall"
	// muffling.
	float outdoor_gain = 1.f;  // 0..1, open path
	float outdoor_hf = 1.f;    // 0..1, open path low-pass gainhf
	float outdoor_wall = 0.f;  // 0..1, wall path
	float outdoor_path = 0.f;  // nodes, cost of the cheapest open path
	v3f outdoor_dir;           // node space, unit; zero when standing under the sky
	float outdoor_focus = 0.f; // 0 wide/ambient .. 1 a single opening

	// The space next door: the biggest region reachable through an opening
	// that isn't the room you're in (a cave beyond the hut's door). Its
	// reverb spills in through the opening, from that direction.
	float coupling = 0.f;       // 0..1, how much of it you hear
	float neighbor_decay = 0.5f;
	float neighbor_wet = 0.f;
	float neighbor_hf_ratio = 1.f;
	v3f neighbor_dir;           // node space, unit; toward the opening

	// Floor under your feet: 1 = planks over empty space (boomy footsteps)
	float hollow = 0.f;

	// What the surfaces are: 1 = rock, earth, snow (a cave, a hillside),
	// 0 = built (planks, glass, wool)
	float natural = 0.f;

	// Set by the sound thread at mix time, not by the probe
	float duck = 0.f;    // 0..1, ambience dips under a loud event
	float deafen = 0.f;  // 0..1, a blast close by: muffled, ringing

	// Listener medium
	bool underwater = false;
	bool in_lava = false;
	float submerged = 0.f;     // 0..1, smoothed immersion (water or lava)
};

/// The sound thread's smoothed state, reported back for the debug HUD and for
/// other client systems (the fx renderer can key exposure and fog on it).
struct Report {
	bool valid = false;
	Acoustics now;    // smoothed values currently applied
	float probe_ms = 0.f;
	u32 sources = 0;
	u32 occluded = 0; // positional sources with occlusion > 0.1
	u32 routed = 0;   // ...of which heard around a corner
	bool efx = false; // EFX available and in use
};

/// Analyse the snapshot from `ear` (node space, float). Pure function.
Acoustics analyze(const VoxelSnapshot &snap, v3f ear, const Params &params);

/// Fraction 0..1 of a direct path blocked between two points (node space),
/// and the high-frequency loss along it. Pure function.
struct Occlusion {
	float amount = 0.f;  // 0 clear .. 1 fully blocked
	float hf = 1.f;      // 0..1 gainhf multiplier on the direct path
	float wall = 0.f;    // 0..1 what gets through the blocker (muffled path)
	// Around the corner: the sound travels through an opening instead.
	float path = 0.f;    // 0..1, how much the bent path is used
	float path_gain = 0.f; // diffraction loss of the bend (distance is in virt_pos)
	float path_hf = 1.f;
	v3f virt_pos;        // node space: heard from here (direction of the opening,
	                     // at the path's length from the ear)
};
Occlusion occlusion(const VoxelSnapshot &snap, v3f from, v3f to);

/// Shortest routes through open air (and leaves, fences, open doors) from
/// the ear, within a window around it. Built per probe; read per source.
struct AirField {
	v3s16 origin;          // node position of index 0
	s16 size = 0;          // window edge
	v3s16 ear;
	std::vector<u16> dist; // quarter nodes; 0xffff = unreachable
	std::vector<u8> parent;// 1..6: index into NEIGHBORS of the step *toward* the ear
	u32 index(v3s16 p) const {
		return (u32)(p.X - origin.X) + (u32)size * ((u32)(p.Y - origin.Y) +
				(u32)size * (u32)(p.Z - origin.Z));
	}
	bool contains(v3s16 p) const {
		return p.X >= origin.X && p.Y >= origin.Y && p.Z >= origin.Z &&
				p.X < origin.X + size && p.Y < origin.Y + size && p.Z < origin.Z + size;
	}
	float pathLength(v3s16 p) const {
		if (!contains(p)) return -1.f;
		u16 d = dist[index(p)];
		return d == 0xffff ? -1.f : d / 4.f;
	}
};

/// Build the field from the ear outward, `radius` nodes each way.
void buildAirField(const VoxelSnapshot &snap, v3f ear, s16 radius, AirField &out);

/// Fill the path fields of `occ` for a source at `src`: the route through
/// open air, where it turns toward the ear, and how much it loses bending.
void routeAround(const VoxelSnapshot &snap, const AirField &field, v3f ear, v3f src,
		Occlusion &occ);

/// The space around a sound's source, not the listener: a zombie in the
/// cavern below rings with the cavern even while you stand in a dead hut.
struct SourceSpace {
	bool valid = false;
	float wet = 0.f;
	float decay = 0.3f;
	float hf_ratio = 1.f;
	float enclosure = 0.f;
	bool in_water = false;   // the source is under water (a drowned, a fish)
};

/// A coarse look at the space around `pos`: few rays, short reach.
SourceSpace analyzeSpace(const VoxelSnapshot &snap, v3f pos, const Params &params);

/// The far space everyone's hearing right now (send 5): the most prominent
/// source space that rings longer than the listener's room.
struct RemoteSpace {
	float level = 0.f;     // 0..1 slot gain
	float decay = 1.f;
	float hf_ratio = 1.f;
	v3f dir;               // node space, unit or zero: toward the source
};

/// Unit directions on a Fibonacci sphere. Cached per count.
const std::vector<v3f> &sphereDirections(int count);

} // namespace afx
