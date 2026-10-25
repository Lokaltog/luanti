// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The acoustic probe. Pure functions over a VoxelSnapshot:
//
//  1. Rays on a Fibonacci sphere give the room's size and what it's made of.
//     Sabine's formula turns that into a decay time, the hit distances into
//     early-reflection and late-reverb delays.
//  2. A coherent cluster of far, hard hits is an echo (a canyon wall, the far
//     side of a cavern).
//  3. A cheapest-path search through the voxels to the nearest sky-lit air
//     gives how muffled the outdoors is and from which direction it arrives
//     (the doorway, the cave mouth). Solid voxels are crossable at a high
//     cost, so outdoor sound leaks through a wooden wall but not a mountain.

#include "afx_types.h"
#include "util/numeric.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace afx
{

// Transmission costs are in "nodes of air": a path's gain is exp(-cost / 6),
// so a plank wall (18) lets about -26 dB through and stone (40) about -58 dB.
static const MaterialProps MATERIALS[MATERIAL_COUNT] = {
	//  name       absorb  hf    pass  pass_hf
	{"air",        0.00f, 1.00f,  1.0f, 0.10f},
	{"leaky",      0.30f, 0.80f,  3.0f, 0.40f},
	{"foliage",    0.50f, 0.35f,  2.5f, 0.35f},
	{"soft",       0.70f, 0.20f, 16.0f, 3.00f},
	{"snow",       0.60f, 0.30f, 16.0f, 3.00f},
	{"glass",      0.04f, 0.95f, 14.0f, 2.00f},
	{"wood",       0.25f, 0.60f, 18.0f, 2.50f},
	{"earth",      0.12f, 0.60f, 30.0f, 3.00f},
	{"stone",      0.09f, 0.90f, 40.0f, 3.50f},
	{"water",      0.03f, 0.90f,  6.0f, 1.50f},
	{"lava",       0.10f, 0.60f, 40.0f, 3.50f},
};

/// Doors, panes and trapdoors are thinner than a voxel.
static constexpr float THIN_PASS = 0.6f;
static constexpr float PATH_LAMBDA = 6.f; // nodes of air per e-fold of gain

const MaterialProps &props(Material m)
{
	return MATERIALS[std::min<int>((int)m, MATERIAL_COUNT - 1)];
}

static constexpr float SPEED_OF_SOUND = 343.f; // nodes per second

template <typename T>
static T clampf(T v, T lo, T hi) { return std::max(lo, std::min(hi, v)); }

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static v3s16 floorNode(v3f p)
{
	return v3s16(std::floor(p.X + 0.5f), std::floor(p.Y + 0.5f), std::floor(p.Z + 0.5f));
}

const std::vector<v3f> &sphereDirections(int count)
{
	static std::mutex mutex;
	static std::unordered_map<int, std::vector<v3f>> cache;
	std::lock_guard<std::mutex> lock(mutex);
	auto &dirs = cache[count];
	if (dirs.empty()) {
		const float golden = M_PI * (3.f - std::sqrt(5.f));
		dirs.reserve(count);
		for (int i = 0; i < count; i++) {
			float y = 1.f - (i + 0.5f) * 2.f / count;
			float r = std::sqrt(std::max(0.f, 1.f - y * y));
			float th = golden * i;
			dirs.emplace_back(std::cos(th) * r, y, std::sin(th) * r);
		}
	}
	return dirs;
}

/*
	Voxel traversal (Amanatides & Woo). Node centers are at integer
	coordinates, so voxel boundaries sit at .5. Calls `visit(cell_pos, t, axis)`
	for every voxel entered after the starting one, where t is the distance
	travelled and axis the one crossed to enter it; stops when visit returns
	false or t exceeds max_t.
*/
template <typename F>
static void traverse(v3f from, v3f dir, float max_t, F &&visit)
{
	v3s16 cell = floorNode(from);
	s16 step[3];
	float t_max[3], t_delta[3];
	const float o[3] = {from.X + 0.5f, from.Y + 0.5f, from.Z + 0.5f};
	const float d[3] = {dir.X, dir.Y, dir.Z};
	s16 *c[3] = {&cell.X, &cell.Y, &cell.Z};
	for (int a = 0; a < 3; a++) {
		if (d[a] > 1e-6f) {
			step[a] = 1;
			t_delta[a] = 1.f / d[a];
			t_max[a] = (std::floor(o[a]) + 1.f - o[a]) * t_delta[a];
		} else if (d[a] < -1e-6f) {
			step[a] = -1;
			t_delta[a] = -1.f / d[a];
			t_max[a] = (o[a] - std::floor(o[a])) * t_delta[a];
		} else {
			step[a] = 0;
			t_delta[a] = INFINITY;
			t_max[a] = INFINITY;
		}
	}
	while (true) {
		int a = (t_max[0] < t_max[1]) ? (t_max[0] < t_max[2] ? 0 : 2)
				: (t_max[1] < t_max[2] ? 1 : 2);
		float t = t_max[a];
		if (t > max_t)
			return;
		*c[a] += step[a];
		t_max[a] += t_delta[a];
		if (!visit(cell, t, a))
			return;
	}
}

/*
	Rays
*/

namespace {

struct RayHit {
	v3f dir;
	float dist;     // distance to the hit, or max distance when escaped
	Material mat;   // Air when escaped
	bool hit;
	bool sky;       // passed through sky-lit air before hitting (or escaped upward into it)
};

} // namespace

static RayHit castRay(const VoxelSnapshot &snap, v3f ear, v3f dir, float max_t)
{
	RayHit r{dir, max_t, Material::Air, false, false};
	traverse(ear, dir, max_t, [&](v3s16 p, float t, int axis) {
		u8 cell = snap.at(p);
		if (cell & CELL_UNLOADED)
			return false; // out of the snapshot: treat as open
		Material m = cellMaterial(cell);
		if (!cellBlocks(cell, axis)) {
			r.sky |= cellSky(cell);
			return true;
		}
		r.dist = std::max(t, 0.5f);
		r.mat = m;
		r.hit = true;
		return false;
	});
	return r;
}

/*
	Outdoor paths: Dial's algorithm (bucket-queue Dijkstra) over the snapshot,
	on two layers. Layer 0 has only gone through porous voxels (air, leaves,
	fences, water); entering a solid moves the search to layer 1. Reaching
	sky on layer 0 is the open path (a doorway), on layer 1 the wall path.
	Costs are in quarter nodes so integer buckets stay exact enough.
*/

namespace {

struct PathResult {
	bool found = false;
	v3s16 exit, inner;  // the first sky cell, the one before it
	int width = 0;      // open cells around `inner` in its plane (5x5)
	float cost = 0.f;   // nodes of air-equivalent
	float hf_loss = 0.f;// nepers
	v3f dir;            // from ear toward the opening along the path
	float steps = 0.f;  // path length in voxels
};

struct PathScratch {
	// One record per (voxel, layer). Valid only when stamp == the current
	// search's generation, so nothing needs clearing between probes.
	struct Node {
		u32 stamp;
		u16 dist;
		u8 parent; // 7 start, else bits 0-2: came along NEIGHBORS[k-1], bit 3: prev layer
		float hf;
	};
	std::vector<Node> nodes;
	u32 gen = 0;
	std::vector<std::vector<u32>> buckets;

	Node &at(u32 i) { return nodes[i]; }
	u16 dist(u32 i) const { return nodes[i].stamp == gen ? nodes[i].dist : 0xffff; }
};

} // namespace

static constexpr int COST_SCALE = 4;
static constexpr int PATH_LIMIT_NODES = 64;
static constexpr u32 PATH_POP_BUDGET = 200000;

static const v3s16 NEIGHBORS[6] = {
	{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

/// Cost of entering `cell` moving along `axis`, and whether that is through
/// a solid (moves the search to the wall layer).
static float passCost(u8 cell, int axis, bool &solid, float &hf)
{
	Material m = cellMaterial(cell);
	const MaterialProps &mp = props(m);
	int thin = cellThinAxis(cell);
	if (m != Material::Air && thin >= 0 && thin != axis) {
		// A thin node edge-on (an open door): like air
		solid = false;
		hf = props(Material::Air).pass_hf;
		return props(Material::Air).pass;
	}
	solid = !materialPorous(m);
	const float k = thin >= 0 ? THIN_PASS : 1.f;
	hf = mp.pass_hf * k;
	return mp.pass * k;
}

/// Open cells connected to `c` in the plane across `axis`, within 3 nodes
/// (so at most 49): what the path could pass, as the path search sees it.
static int crossSection(const VoxelSnapshot &snap, v3s16 c, int axis)
{
	auto passable = [&](v3s16 q) {
		if (!snap.contains(q))
			return false;
		const u8 cell = snap.at(q);
		if (cell & CELL_UNLOADED)
			return false;
		return !cellBlocks(cell, axis) || materialPorous(cellMaterial(cell));
	};
	if (!passable(c))
		return 1;
	v3s16 queue[49];
	bool seen[7][7] = {};
	int head = 0, tail = 0;
	auto uv = [&](v3s16 q, int &u, int &v) {
		const v3s16 d = q - c;
		u = (axis == 0 ? d.Y : d.X) + 3;
		v = (axis == 2 ? d.Y : d.Z) + 3;
	};
	queue[tail++] = c;
	seen[3][3] = true;
	while (head < tail) {
		const v3s16 q = queue[head++];
		for (int k = 0; k < 6; k++) {
			if (k / 2 == axis)
				continue; // stay in the plane
			const v3s16 n = q + NEIGHBORS[k];
			int u, v;
			uv(n, u, v);
			if (u < 0 || u > 6 || v < 0 || v > 6 || seen[u][v] || !passable(n))
				continue;
			seen[u][v] = true;
			queue[tail++] = n;
		}
	}
	return tail;
}

static void findSky(const VoxelSnapshot &snap, v3s16 start, v3f ear,
		PathResult &open, PathResult &wall)
{
	open = wall = PathResult();
	if (!snap.contains(start))
		return;

	thread_local PathScratch s;
	const u32 n = (u32)snap.cells.size();
	if (s.nodes.size() != 2 * (size_t)n) {
		s.nodes.assign(2 * (size_t)n, PathScratch::Node{0, 0xffff, 0, 0.f});
		s.gen = 0;
	}
	if (++s.gen == 0) { // wrapped: clear once every 4 billion probes
		for (auto &nd : s.nodes)
			nd.stamp = 0;
		s.gen = 1;
	}

	int max_edge = 0;
	for (const auto &m : MATERIALS)
		max_edge = std::max(max_edge, (int)std::lround(m.pass * COST_SCALE));
	const int nb = max_edge + 1;
	s.buckets.resize(nb);
	for (auto &b : s.buckets)
		b.clear();

	const int limit = PATH_LIMIT_NODES * COST_SCALE;
	const u32 si = snap.index(start);
	s.at(si) = {s.gen, 0, 7, 0.f};
	s.buckets[0].push_back(si);
	size_t pending = 1;
	u32 pops = 0;
	u32 found[2] = {U32_MAX, U32_MAX};

	auto pos_of = [&](u32 i) {
		i %= n;
		s16 x = i % snap.size, y = (i / snap.size) % snap.size, z = i / (snap.size * snap.size);
		return snap.origin + v3s16(x, y, z);
	};

	for (int cost = 0; pending > 0 && cost <= limit &&
			(found[0] == U32_MAX || found[1] == U32_MAX); cost++) {
		auto &bucket = s.buckets[cost % nb];
		while (!bucket.empty()) {
			u32 i = bucket.back();
			bucket.pop_back();
			pending--;
			if (s.dist(i) != cost)
				continue; // stale entry
			if (++pops > PATH_POP_BUDGET) {
				pending = 0;
				break;
			}
			const u32 layer = i / n;
			const u32 ci = i % n;
			if (cellSky(snap.cells[ci])) {
				if (found[layer] == U32_MAX)
					found[layer] = i;
				continue; // don't expand past the sky
			}
			v3s16 p = pos_of(i);
			for (int k = 0; k < 6; k++) {
				v3s16 q = p + NEIGHBORS[k];
				if (!snap.contains(q))
					continue;
				u32 qc = snap.index(q);
				bool solid;
				float hf;
				float pc = passCost(snap.cells[qc], k / 2, solid, hf);
				u32 ql = (layer == 1 || solid) ? 1 : 0;
				u32 qi = qc + ql * n;
				int nc = cost + (int)std::lround(pc * COST_SCALE);
				if (nc > limit || nc >= s.dist(qi))
					continue;
				s.at(qi) = {s.gen, (u16)nc, (u8)((k + 1) | (layer << 3)), s.at(i).hf + hf};
				s.buckets[nc % nb].push_back(qi);
				pending++;
			}
		}
	}

	for (int layer = 0; layer < 2; layer++) {
		u32 f = found[layer];
		if (f == U32_MAX)
			continue;
		PathResult &res = layer == 0 ? open : wall;
		res.found = true;
		res.cost = s.dist(f) / (float)COST_SCALE;
		res.hf_loss = s.at(f).hf;

		// Walk back to the ear. The direction sound arrives from is where the
		// path is a few nodes out: the doorway rather than the sky.
		std::vector<v3s16> path;
		u32 i = f;
		while (s.at(i).parent != 7 && path.size() < 4096) {
			v3s16 p = pos_of(i);
			path.push_back(p);
			const u8 par = s.at(i).parent;
			v3s16 prev = p - NEIGHBORS[(par & 7) - 1];
			i = snap.index(prev) + (u32)(par >> 3) * n;
		}
		res.steps = (float)path.size();
		if (!path.empty()) {
			// The doorway: the narrowest cross-section over the last steps
			// before the sky (under a porch the first sky cell is out past
			// the eaves, and the cells there are wide open; the door in the
			// wall is the narrow one). Measured by a flood fill across the
			// step, through what the path itself passes (air, leaves,
			// fences, water), so a window two cells off doesn't count.
			res.exit = path[0];
			res.inner = path.size() > 1 ? path[1] : start;
			res.width = 1 << 30;
			const size_t last = std::min<size_t>(path.size() - 1, 6);
			for (size_t k = 1; k <= std::max<size_t>(last, 1); k++) {
				const v3s16 cell = k < path.size() ? path[k] : start;
				const v3s16 step = path[k - 1] - cell;
				const int axis = step.X != 0 ? 0 : step.Y != 0 ? 1 : 2;
				const int w = crossSection(snap, cell, axis);
				if (w < res.width) {
					res.width = w;
					res.inner = cell;
				}
			}
			size_t k = path.size() > 5 ? path.size() - 5 : 0;
			v3f d = intToFloat(path[k], 1.f) - ear;
			if (d.getLengthSQ() > 1e-4f)
				res.dir = d.normalize();
		}
	}
}

/*
	Echo cluster: far, hard hits that agree in direction and distance.
*/

namespace {
struct EchoCluster {
	float weight = 0.f; // fraction of rays
	float dist = 0.f;
	v3f dir;
	float hf = 1.f;
};
}

static constexpr float ECHO_MIN_DIST = 10.f;
static constexpr float ECHO_MAX_ABSORPTION = 0.15f;

static EchoCluster findEcho(const std::vector<RayHit> &rays, v3f around = v3f(), bool opposite = false)
{
	EchoCluster best;
	const float n = (float)rays.size();
	for (const auto &a : rays) {
		if (!a.hit || a.dist < ECHO_MIN_DIST || props(a.mat).absorption > ECHO_MAX_ABSORPTION)
			continue;
		if (opposite && a.dir.dotProduct(around) > -0.8f)
			continue;
		float w = 0.f, dsum = 0.f, hsum = 0.f;
		v3f vsum;
		for (const auto &b : rays) {
			if (!b.hit || b.dist < ECHO_MIN_DIST || props(b.mat).absorption > ECHO_MAX_ABSORPTION)
				continue;
			// ~37° cone, distances within 30%: one wall seen at a slant still
			// reads as one reflector
			if (a.dir.dotProduct(b.dir) < 0.8f || std::fabs(a.dist - b.dist) > 0.3f * a.dist)
				continue;
			float bw = 1.f - props(b.mat).absorption;
			w += bw;
			dsum += b.dist * bw;
			hsum += props(b.mat).hf * bw;
			vsum += b.dir * bw;
		}
		if (w / n > best.weight) {
			best.weight = w / n;
			best.dist = dsum / w;
			best.hf = hsum / w;
			best.dir = vsum.getLengthSQ() > 0 ? vsum.normalize() : a.dir;
		}
	}
	return best;
}

/*
	The analysis
*/

static void analyzeNeighbor(const VoxelSnapshot &snap, v3f ear, const Params &params,
		Acoustics &out);
static float floorHollowness(const VoxelSnapshot &snap, v3s16 ear_cell);

static Acoustics analyzeCore(const VoxelSnapshot &snap, v3f ear, const Params &params, bool full);

SourceSpace analyzeSpace(const VoxelSnapshot &snap, v3f pos, const Params &params)
{
	SourceSpace out;
	if (snap.size <= 0 || !snap.contains(floorNode(pos)))
		return out;
	Params sub = params;
	sub.rays = 24;
	sub.probe_radius = std::min<s16>(params.probe_radius, 18);
	const Acoustics a = analyzeCore(snap, pos, sub, false);
	out.valid = true;
	out.wet = a.wet;
	out.decay = a.decay;
	out.hf_ratio = a.decay_hf_ratio;
	out.enclosure = a.enclosure;
	out.in_water = a.underwater;
	return out;
}

std::vector<DebugRay> probeRays(const VoxelSnapshot &snap, v3f ear, int rays)
{
	const float max_t = std::max(4.f, snap.size * 0.5f - 1.f);
	std::vector<DebugRay> out;
	for (const v3f &d : sphereDirections(std::max(8, rays))) {
		const RayHit r = castRay(snap, ear, d, max_t);
		out.push_back({ear + d * r.dist, r.mat, r.hit, r.sky});
	}
	return out;
}

Acoustics analyze(const VoxelSnapshot &snap, v3f ear, const Params &params)
{
	return analyzeCore(snap, ear, params, true);
}

static Acoustics analyzeCore(const VoxelSnapshot &snap, v3f ear, const Params &params, bool full)
{
	Acoustics out;
	if (snap.size <= 0)
		return out;

	// The camera can clip into a node in third person; listen from the
	// nearest open neighbor instead.
	v3s16 ear_cell = floorNode(ear);
	{
		Material m = cellMaterial(snap.at(ear_cell));
		out.underwater = m == Material::Water;
		out.in_lava = m == Material::Lava;
		out.submerged = (out.underwater || out.in_lava) ? 1.f : 0.f;
		if (out.underwater) {
			int d = 1;
			while (d < 8 && cellMaterial(snap.at(ear_cell + v3s16(0, d, 0))) == Material::Water)
				d++;
			out.water_depth = (float)d;
		}
		if (m != Material::Air && !out.underwater && !out.in_lava) {
			for (const auto &nb : NEIGHBORS) {
				if (cellMaterial(snap.at(ear_cell + nb)) == Material::Air) {
					ear_cell += nb;
					ear = intToFloat(ear_cell, 1.f);
					break;
				}
			}
		}
	}

	const float max_t = std::max(4.f, (float)params.probe_radius - 1.f);
	const auto &dirs = sphereDirections(std::max(8, params.rays));
	std::vector<RayHit> rays;
	rays.reserve(dirs.size());
	for (const v3f &d : dirs)
		rays.push_back(castRay(snap, ear, d, max_t));

	const float n = (float)rays.size();
	float hits = 0, sky = 0, enclosed_w = 0;
	float sum_d = 0;
	float sum_abs = 0, hit_abs = 0, hit_hf = 0, glass = 0;
	float hit_d = 0;
	float natural = 0;
	float far_hits = 0; // hits beyond arm's reach: terrain that makes a tail
	std::vector<float> hit_dists;
	v3f pan;
	float pan_w = 0;
	v3f sky_dir;

	for (const auto &r : rays) {
		float d = r.dist;
		sum_d += d;
		if (r.hit) {
			const MaterialProps &mp = props(r.mat);
			hits += 1;
			sum_abs += mp.absorption;
			hit_abs += mp.absorption;
			hit_hf += mp.hf;
			if (d > 3.f)
				far_hits += 1;
			if (r.mat == Material::Stone || r.mat == Material::Earth ||
					r.mat == Material::Snow || r.mat == Material::Lava || r.mat == Material::Water)
				natural += 1;
			hit_d += d;
			hit_dists.push_back(d);
			if (r.mat == Material::Glass)
				glass += 1;
			// Hits beyond half the probe radius only half enclose.
			enclosed_w += clampf(2.f - 2.f * d / max_t, 0.f, 1.f);
			float w = (1.f - mp.absorption) / std::max(1.f, d * d);
			pan += r.dir * w;
			pan_w += w;
		} else {
			sum_abs += 1.f; // energy that leaves never comes back
		}
		if (r.sky || !r.hit) {
			sky += 1;
			sky_dir += r.dir;
		}
	}

	const float avg_abs = clampf(sum_abs / n, 0.01f, 1.f);
	const float hit_abs_avg = hits > 0 ? hit_abs / hits : 1.f;
	const float hit_hf_avg = hits > 0 ? hit_hf / hits : 1.f;
	const float hit_d_avg = hits > 0 ? hit_d / hits : max_t;

	// Half the sphere hits the ground in an open field, so 0.5 is "open".
	out.enclosure = clampf((enclosed_w / n - 0.5f) * 2.f, 0.f, 1.f);
	out.mean_free_path = sum_d / n;
	out.absorption = avg_abs;
	out.natural = hits > 0 ? natural / hits : 0.f;
	out.hf = hit_hf_avg;

	// Sabine: RT60 = 0.161 V / (S a). Treating the rays as samples of a
	// star-shaped room, V/S = E[d³] / (3 E[d²]).
	// Sabine: RT60 = 0.161 V / (S a), with V/S from the mean hit distance
	// (a sphere seen from its centre has V/S = r/3). The mean is robust where
	// a cubic moment is not: one long ray down a corridor doesn't make the
	// corridor a cathedral. Escaped rays only add absorption.
	const float v_over_s = (hits > 0 ? hit_d / hits : max_t) / 3.f;
	const float rt60 = 0.161f * v_over_s / avg_abs;
	// Open land still has a faint, long, distant tail (the "big outdoors"):
	// the ground and hills further off. Standing on a skybridge or a pillar
	// there is none: the planks right under you are a reflection, not a tail.
	// A meadow has about a quarter of the rays hitting ground beyond 3 nodes.
	const float terrain = clampf((far_hits / n) / 0.22f, 0.f, 1.f);
	const float open_tail = lerpf(1.8f * terrain, 0.f, out.enclosure);
	out.decay = clampf(std::max(rt60, open_tail), 0.1f, std::max(0.2f, params.max_decay));
	out.decay_hf_ratio = clampf(0.3f + 0.9f * hit_hf_avg, 0.1f, 2.f);
	out.density = clampf(1.2f - out.mean_free_path / 20.f, 0.3f, 1.f);
	out.diffusion = clampf(1.f - 0.5f * (hits > 0 ? glass / hits : 0.f), 0.f, 1.f);

	std::sort(hit_dists.begin(), hit_dists.end());
	float near_d = 1.f;
	if (!hit_dists.empty()) {
		size_t k = std::max<size_t>(1, hit_dists.size() / 4);
		float s = 0;
		for (size_t i = 0; i < k; i++)
			s += hit_dists[i];
		near_d = s / k;
	}
	out.reflections_delay = clampf(2.f * near_d / SPEED_OF_SOUND, 0.f, 0.3f);
	out.late_delay = clampf(hit_d_avg / SPEED_OF_SOUND, 0.f, 0.1f);

	// Dead rooms (wool, leaves) hold little reverberant energy; stone holds most.
	const float liveness = std::pow(1.f - hit_abs_avg, 4.f);
	out.reflections_gain = clampf((0.05f + out.enclosure * (1.f - hit_abs_avg)) *
			clampf(6.f / std::max(1.f, hit_d_avg), 0.2f, 1.5f), 0.f, 3.16f);
	out.late_gain = clampf(1.2f * (1.f - hit_abs_avg), 0.2f, 3.f);
	out.wet = clampf(params.reverb * lerpf(0.3f * terrain, 1.f, out.enclosure) * liveness, 0.f, 1.f);
	if (pan_w > 0) {
		v3f p = pan / pan_w;
		out.reflections_pan = p * std::min(1.f, 1.f - out.enclosure * 0.5f);
	}

	if (!full)
		return out; // a neighbour's room: the reverb is all we need

	// Walls beside you: the nearest roughly horizontal surface that's a wall
	// and not a post: several rays agreeing on it (a cone of ~30 degrees)
	{
		auto flat = [](const RayHit &r) { return r.dist * std::sqrt(std::max(0.f, 1.f - r.dir.Y * r.dir.Y)); };
		auto usable = [&](const RayHit &r, float maxd) {
			return r.hit && std::fabs(r.dir.Y) < 0.35f && flat(r) <= maxd && props(r.mat).absorption < 0.4f;
		};
		// how many rays agree on it, and only if it spreads sideways (a wall,
		// not a trunk or a post): rays at least ~17 degrees apart around you
		auto cluster = [&](const RayHit &c) {
			int k = 0;
			float hx = c.dir.X, hz = c.dir.Z;
			const float hl = std::sqrt(hx * hx + hz * hz);
			if (hl < 1e-4f)
				return 0;
			hx /= hl;
			hz /= hl;
			float widest = 1.f;
			for (const auto &r : rays) {
				if (!usable(r, 16.f) || r.dir.dotProduct(c.dir) < 0.8f ||
						std::fabs(flat(r) - flat(c)) > 0.35f * flat(c) + 0.5f)
					continue;
				k++;
				const float rl = std::sqrt(r.dir.X * r.dir.X + r.dir.Z * r.dir.Z);
				if (rl > 1e-4f)
					widest = std::min(widest, (r.dir.X * hx + r.dir.Z * hz) / rl);
			}
			return widest < 0.94f ? k : 0;
		};
		const RayHit *near = nullptr, *far = nullptr;
		for (const auto &r : rays) {
			if (!usable(r, 14.f) || cluster(r) < 3)
				continue;
			if (!near || flat(r) < flat(*near))
				near = &r;
		}
		if (near) {
			for (const auto &r : rays) {
				if (!usable(r, 16.f) || r.dir.dotProduct(near->dir) > -0.6f || cluster(r) < 3)
					continue;
				if (!far || flat(r) < flat(*far))
					far = &r;
			}
			auto strength = [&](const RayHit &r) {
				const MaterialProps &mp = props(r.mat);
				const float share = clampf(cluster(r) / 5.f, 0.f, 1.f);
				return clampf((1.f - mp.absorption) * share * clampf((12.f - flat(r)) / 10.f, 0.f, 1.f),
						0.f, 1.f);
			};
			out.wall_near = strength(*near);
			out.wall_near_dist = std::max(0.5f, flat(*near));
			out.wall_near_dir = near->dir;
			if (far) {
				out.wall_far = strength(*far);
				out.wall_far_dist = std::max(0.5f, flat(*far));
				out.wall_far_dir = far->dir;
			}
		}
	}

	// Echo
	EchoCluster echo = findEcho(rays);
	if (echo.weight > 0) {
		// A distinct echo needs open air around the listener (a cliff, a
		// canyon). Inside a cavern the same surfaces are the reverb.
		const float openness = (1.f - out.enclosure) * (1.f - out.enclosure);
		float strength = clampf((echo.weight - 0.02f) / 0.08f, 0.f, 1.f) *
				clampf(25.f / echo.dist, 0.4f, 1.f) * openness;
		out.echo = clampf(strength * params.echo, 0.f, 1.f);
		out.echo_delay = clampf(2.f * echo.dist / SPEED_OF_SOUND, 0.02f, 0.207f);
		out.echo_damping = clampf(1.f - echo.hf, 0.f, 0.99f);
		out.echo_dir = echo.dir;
		EchoCluster back = findEcho(rays, echo.dir, true);
		// Parallel walls (a canyon) ring a little; a single cliff doesn't
		out.echo_feedback = back.weight > 0.04f ? 0.2f : 0.f;

		// A single echo is the room's first reflection: carry it on the
		// reverb's early reflections, which have a delay (up to 0.3 s) and a
		// direction. The echo slot only adds flutter between parallel walls.
		if (out.echo > 0.05f) {
			const float t = clampf(2.f * echo.dist / SPEED_OF_SOUND, 0.f, 0.3f);
			out.reflections_delay = lerpf(out.reflections_delay, t, std::min(1.f, out.echo * 2.f));
			out.reflections_pan = echo.dir * 0.9f;
			const float wet_level = std::max(0.02f, out.wet * 0.32f);
			out.reflections_gain = clampf(std::max(out.reflections_gain,
					out.echo * 0.5f / wet_level), 0.f, 3.16f);
		}
	}

	// Outdoors
	const float k = std::max(0.f, params.outdoor_muffle);
	if (out.underwater || out.in_lava) {
		// just under the surface the world above still reaches you, dull;
		// a few nodes down only a rumble does
		const bool shallow = out.underwater && out.water_depth <= 2.f;
		out.outdoor_gain = shallow ? (out.water_depth <= 1.f ? 0.3f : 0.12f) : 0.f;
		out.outdoor_hf = shallow ? 0.2f : 0.05f;
		out.outdoor_wall = 0.2f; // dull rumble through the water
	} else if (cellSky(snap.at(ear_cell))) {
		out.outdoor_gain = 1.f;
		out.outdoor_hf = 1.f;
		out.outdoor_wall = 0.f;
		out.outdoor_path = 0.f;
		out.outdoor_focus = 0.f;
	} else {
		PathResult open, wall;
		findSky(snap, ear_cell, ear, open, wall);
		const float sky_frac = sky / n;
		if (open.found) {
			// How much of the outdoors the opening shows: a doorway or a
			// tunnel mouth is a small window on the world, however close it
			// is. The share of rays that get out, against the half that do
			// on open ground. A porch keeps most; a cave mouth -15 dB or so.
			// The path loss covers long tunnels; this covers short ones,
			// where the way out is near but narrow: the smaller wins.
			const float aperture = std::max(0.08f,
					std::pow(clampf(sky_frac / 0.5f, 0.f, 1.f), 0.5f));
			out.outdoor_path = open.cost;
			out.outdoor_gain = std::min(std::exp(-open.cost * k / PATH_LAMBDA),
					lerpf(1.f, aperture, std::min(1.f, k)));
			// a small opening passes the lows round its edges, not the highs
			out.outdoor_hf = std::exp(-open.hf_loss * k) *
					lerpf(1.f, 0.4f + 0.6f * aperture, std::min(1.f, k));
			out.outdoor_dir = open.dir;
			out.has_opening = 1.f;
			out.opening = intToFloat(open.exit, 1.f);
			out.opening_inner = intToFloat(open.inner, 1.f);
			out.opening_size = (float)open.width;
			out.outdoor_focus = clampf(1.f - 2.f * sky_frac, 0.f, 1.f) *
					clampf(open.steps / 4.f, 0.f, 1.f);
		} else {
			out.outdoor_gain = 0.f;
			out.outdoor_hf = 0.f;
			out.outdoor_path = (float)PATH_LIMIT_NODES;
			out.outdoor_focus = 1.f;
		}
		if (wall.found) {
			// Only what the wall path adds beyond the open path
			out.outdoor_wall = std::exp(-wall.cost * k / PATH_LAMBDA);
			if (!open.found)
				out.outdoor_dir = wall.dir;
		}
	}

	if (!out.submerged) {
		analyzeNeighbor(snap, ear, params, out);
		out.hollow = floorHollowness(snap, ear_cell);
	}
	return out;
}

/*
	Occlusion of a positional source
*/

static float blockedCost(const VoxelSnapshot &snap, v3f from, v3f to, float &hf_loss)
{
	v3f d = to - from;
	float len = d.getLength();
	hf_loss = 0.f;
	if (len < 1e-3f)
		return 0.f;
	d /= len;
	v3s16 end = floorNode(to);
	float cost = 0.f;
	traverse(from, d, len, [&](v3s16 p, float, int axis) {
		if (p == end)
			return false; // the emitter's own node doesn't block it
		u8 cell = snap.at(p);
		if (cellBlocks(cell, axis)) {
			const float k = cellThinAxis(cell) >= 0 ? THIN_PASS : 1.f;
			cost += props(cellMaterial(cell)).pass * k;
			hf_loss += props(cellMaterial(cell)).pass_hf * k;
		}
		return cost < 160.f;
	});
	return cost;
}

Occlusion occlusion(const VoxelSnapshot &snap, v3f from, v3f to)
{
	// A sound set into a block (rain drumming in a ceiling plank, a furnace)
	// is heard from its open face toward the listener, not through the
	// blocks beside it.
	{
		const v3s16 c = floorNode(to);
		const u8 cell = snap.at(c);
		if (cellBlocks(cell, 0) || cellBlocks(cell, 1) || cellBlocks(cell, 2)) {
			const v3f toward = from - intToFloat(c, 1.f);
			float best = -2.f;
			v3s16 bestd(0, 0, 0);
			for (const auto &nb : NEIGHBORS) {
				const u8 n = snap.at(c + nb);
				if (cellBlocks(n, 0) || cellBlocks(n, 1) || cellBlocks(n, 2))
					continue;
				const float dot = toward.getLengthSQ() > 1e-6f ?
						intToFloat(nb, 1.f).dotProduct(v3f(toward).normalize()) : 0.f;
				if (dot > best) {
					best = dot;
					bestd = nb;
				}
			}
			if (best > -2.f)
				to = intToFloat(c, 1.f) + intToFloat(bestd, 0.55f);
		}
	}
	// Five rays: the direct one plus four ending a node off the emitter on
	// the plane facing the listener. Averaging the two clearest lets sound
	// bend around a pillar or a door frame (cheap diffraction).
	v3f d = to - from;
	if (d.getLengthSQ() < 1e-4f)
		return {};
	d.normalize();
	v3f up(0, 1, 0);
	v3f side = d.crossProduct(up);
	if (side.getLengthSQ() < 1e-4f)
		side = v3f(1, 0, 0);
	side.normalize();
	v3f vert = side.crossProduct(d).normalize();

	const v3f ends[5] = {to, to + side, to - side, to + vert, to - vert};
	float costs[5], hfs[5];
	for (int i = 0; i < 5; i++)
		costs[i] = blockedCost(snap, from, ends[i], hfs[i]);

	if (costs[0] <= 0.f)
		return {};

	int order[5] = {0, 1, 2, 3, 4};
	std::sort(order, order + 5, [&](int a, int b) { return costs[a] < costs[b]; });
	const float cost = 0.5f * (costs[order[0]] + costs[order[1]]);
	const float hf = 0.5f * (hfs[order[0]] + hfs[order[1]]);

	Occlusion o;
	o.amount = 1.f - std::exp(-cost / 6.f);
	o.hf = std::exp(-hf);
	// What's transmitted through the blocker on the direct line
	o.wall = std::exp(-costs[0] / PATH_LAMBDA);
	return o;
}

/*
	Paths through open air
*/

static constexpr s16 FIELD_RADIUS = 20;
static constexpr int FIELD_LIMIT = 48 * COST_SCALE; // quarter nodes

/// Cost (quarter nodes) of stepping into `cell` along `axis`, or -1 if sound
/// can't travel through it as air.
static int airCost(u8 cell, int axis)
{
	if (cell & CELL_UNLOADED)
		return COST_SCALE;
	const Material m = cellMaterial(cell);
	const int thin = cellThinAxis(cell);
	if (m != Material::Air && thin >= 0 && thin != axis)
		return COST_SCALE; // an open door, edge-on
	switch (m) {
	case Material::Air: return COST_SCALE;
	case Material::Leaky: return 2 * COST_SCALE;
	case Material::Foliage: return 10;
	case Material::Water: return 3 * COST_SCALE;
	default: return -1;
	}
}

namespace {
thread_local std::vector<u32> t_field_order; // pop order, for one-pass labelling
}

void buildAirField(const VoxelSnapshot &snap, v3f ear, s16 radius, AirField &f)
{
	f.size = radius * 2 + 1;
	f.ear = floorNode(ear);
	f.origin = f.ear - v3s16(radius, radius, radius);
	const size_t n = (size_t)f.size * f.size * f.size;
	f.dist.assign(n, 0xffff);
	f.parent.assign(n, 0);
	t_field_order.clear();

	const int nb = 3 * COST_SCALE + 1;
	thread_local std::vector<std::vector<u32>> buckets;
	buckets.resize(nb);
	for (auto &b : buckets)
		b.clear();

	auto pos_of = [&](u32 i) {
		s16 x = i % f.size, y = (i / f.size) % f.size, z = i / (f.size * f.size);
		return f.origin + v3s16(x, y, z);
	};
	const u32 e = f.index(f.ear);
	f.dist[e] = 0;
	buckets[0].push_back(e);
	size_t pending = 1;
	for (int cost = 0; pending > 0 && cost <= FIELD_LIMIT; cost++) {
		auto &bucket = buckets[cost % nb];
		while (!bucket.empty()) {
			u32 i = bucket.back();
			bucket.pop_back();
			pending--;
			if (f.dist[i] != cost)
				continue;
			t_field_order.push_back(i);
			const v3s16 p = pos_of(i);
			for (int k = 0; k < 6; k++) {
				const v3s16 q = p + NEIGHBORS[k];
				if (!f.contains(q))
					continue;
				const int c = airCost(snap.at(q), k / 2);
				if (c < 0)
					continue;
				const u32 qi = f.index(q);
				const int nc = cost + c;
				if (nc > FIELD_LIMIT || nc >= f.dist[qi])
					continue;
				f.dist[qi] = (u16)nc;
				f.parent[qi] = (u8)((k ^ 1) + 1); // the step back toward the ear
				buckets[nc % nb].push_back(qi);
				pending++;
			}
		}
	}
}

static bool visibleFrom(const VoxelSnapshot &snap, v3f from, v3f to)
{
	float hf;
	return blockedCost(snap, from, to, hf) <= 0.f;
}

void routeAround(const VoxelSnapshot &snap, const AirField &f, v3f ear, v3f src,
		Occlusion &occ)
{
	occ.path = 0.f;
	v3s16 s = floorNode(src);
	float L = f.pathLength(s);
	if (L < 0.f) {
		// the emitter sits in a block (a furnace, a mob's feet): start from
		// its best open neighbour
		for (const auto &d : NEIGHBORS) {
			float l = f.pathLength(s + d);
			if (l >= 0.f && (L < 0.f || l < L)) {
				L = l + 1.f;
				s = s + d;
			}
		}
		if (L < 0.f)
			return;
	}

	// Walk back toward the ear; the first cell the ear can see is where the
	// sound turns the corner (the doorway, the tunnel bend).
	v3s16 c = s, turn = f.ear;
	bool found = false;
	for (int steps = 0; steps < 256 && c != f.ear; steps++) {
		if (visibleFrom(snap, ear, intToFloat(c, 1.f))) {
			turn = c;
			found = true;
			break;
		}
		const u8 par = f.parent[f.index(c)];
		if (!par)
			break;
		c = c + NEIGHBORS[par - 1];
	}
	if (!found || turn == s)
		return; // in plain sight after all (from its open face)

	const v3f tp = intToFloat(turn, 1.f);
	v3f dir = tp - ear;
	const float d_turn = dir.getLength();
	if (d_turn < 1e-3f)
		return;
	dir /= d_turn;
	v3f onward = intToFloat(s, 1.f) - tp;
	const float d_on = onward.getLength();
	float bend = 0.f;
	if (d_on > 0.5f) {
		onward /= d_on;
		bend = clampf((1.f - dir.dotProduct(onward)) / 1.5f, 0.f, 1.f);
	}
	occ.virt_pos = ear + dir * std::max(L, d_turn);
	occ.path_gain = std::exp(-1.2f * bend);
	occ.path_hf = std::exp(-2.2f * bend);
	// Use the bent path when it carries more than the wall does
	occ.path = occ.path_gain >= occ.wall * 0.5f ? 1.f : 0.f;
}

/*
	The space next door
*/

static void analyzeNeighbor(const VoxelSnapshot &snap, v3f ear, const Params &params,
		Acoustics &out)
{
	thread_local AirField f;
	buildAirField(snap, ear, FIELD_RADIUS, f);
	const v3f e = intToFloat(f.ear, 1.f);

	// A cell is in "this room" when the way there is about as short as the
	// straight line; otherwise it is around a corner, in a neighbouring
	// space. Label each neighbour cell with the opening it came through, in
	// one pass over the cells in the order they were reached.
	const auto &order = t_field_order;
	thread_local std::vector<u32> portal;
	portal.assign(f.dist.size(), U32_MAX);
	auto pos_of = [&](u32 i) {
		s16 x = i % f.size, y = (i / f.size) % f.size, z = i / (f.size * f.size);
		return f.origin + v3s16(x, y, z);
	};
	auto same_room = [&](u32 i) {
		const float L = f.dist[i] / (float)COST_SCALE;
		const float d = (intToFloat(pos_of(i), 1.f) - e).getLength();
		return L <= d * 1.15f + 1.5f;
	};
	std::unordered_map<u32, u32> per_portal;
	u32 room = 0, beyond = 0;
	for (u32 i : order) {
		if (same_room(i)) {
			room++;
			continue;
		}
		const u8 par = f.parent[i];
		if (!par)
			continue;
		const u32 pi = f.index(pos_of(i) + NEIGHBORS[par - 1]);
		portal[i] = portal[pi] != U32_MAX ? portal[pi] : pi;
		beyond++;
		// group openings on a coarse grid (a 1x2 door is one opening)
		const v3s16 pp = pos_of(portal[i]);
		const u32 key = ((u32)(pp.X >> 1) & 0x3ff) | (((u32)(pp.Y >> 1) & 0x3ff) << 10) |
				(((u32)(pp.Z >> 1) & 0x3ff) << 20);
		per_portal[key]++;
	}
	if (beyond < 40 || per_portal.empty())
		return;

	// The biggest space beyond one opening
	u32 best_key = 0, best_n = 0;
	for (const auto &[k, n] : per_portal)
		if (n > best_n)
			best_key = k, best_n = n;

	v3f sum_portal, sum_cells;
	u32 n_cells = 0;
	std::unordered_map<u32, bool> portal_cells;
	for (u32 i : order) {
		if (portal[i] == U32_MAX)
			continue;
		const v3s16 pp = pos_of(portal[i]);
		const u32 key = ((u32)(pp.X >> 1) & 0x3ff) | (((u32)(pp.Y >> 1) & 0x3ff) << 10) |
				(((u32)(pp.Z >> 1) & 0x3ff) << 20);
		if (key != best_key)
			continue;
		sum_cells += intToFloat(pos_of(i), 1.f);
		n_cells++;
		if (!portal_cells.count(portal[i])) {
			portal_cells[portal[i]] = true;
			sum_portal += intToFloat(pp, 1.f);
		}
	}
	const v3f door = sum_portal / (float)portal_cells.size();
	const v3f centroid = sum_cells / (float)n_cells;

	// Listen from inside it: the cell of that space nearest its middle
	v3f probe = centroid;
	float best_d = 1e9f;
	for (u32 i : order) {
		if (portal[i] == U32_MAX)
			continue;
		const v3f p = intToFloat(pos_of(i), 1.f);
		const float d = (p - centroid).getLengthSQ();
		if (d < best_d)
			best_d = d, probe = p;
	}
	Params sub = params;
	sub.rays = std::max(16, params.rays / 2);
	const Acoustics there = analyzeCore(snap, probe, sub, false);

	const float aperture = std::min(1.f, std::sqrt(portal_cells.size() / 4.f));
	const float size = clampf(best_n / 300.f, 0.f, 1.f);
	const float near = clampf(1.f - ((door - e).getLength() - 2.f) / 14.f, 0.f, 1.f);
	// A space like the one you're in adds little; a ringing cave next to a
	// dead hut is what you notice
	const float contrast = clampf((there.decay - out.decay) / std::max(0.3f, out.decay), 0.f, 1.f);
	out.coupling = aperture * size * near * (0.3f + 0.7f * contrast);
	out.neighbor_decay = there.decay;
	out.neighbor_wet = there.wet;
	out.neighbor_hf_ratio = there.decay_hf_ratio;
	v3f dd = door - e;
	if (dd.getLengthSQ() > 1e-4f)
		out.neighbor_dir = dd.normalize();
}

/*
	The floor under your feet
*/

static float floorHollowness(const VoxelSnapshot &snap, v3s16 ear_cell)
{
	// The floor: first blocking cell below the ear
	v3s16 fl = ear_cell;
	int k = 0;
	for (; k < 4; k++) {
		fl.Y -= 1;
		if (cellBlocks(snap.at(fl), 1))
			break;
	}
	if (k == 4)
		return 0.f;
	const Material m = cellMaterial(snap.at(fl));
	float mat;
	switch (m) {
	case Material::Wood: case Material::Leaky: mat = 1.f; break;
	case Material::Glass: mat = 0.7f; break;
	case Material::Stone: mat = 0.45f; break;
	case Material::Earth: mat = 0.3f; break;
	case Material::Soft: case Material::Snow: return 0.f; // a carpet, wool: dead
	default: mat = 0.2f;
	}
	if (cellThinAxis(snap.at(fl)) >= 0 && m != Material::Earth)
		mat = std::max(mat, 0.9f); // a slab or a trapdoor

	// Thickness, then how much empty space lies under it
	int thick = 1;
	v3s16 b = fl;
	while (thick < 3 && cellBlocks(snap.at(b - v3s16(0, 1, 0)), 1)) {
		b.Y -= 1;
		thick++;
	}
	if (thick > 2)
		return 0.f;
	// Only under the floor itself: columns whose floor cell blocks too (not
	// the air past a doorstep or a raised edge), and not open sky (a bridge's
	// underside is the outdoors, not a cavity)
	int air = 0, total = 0;
	for (s16 dz = -1; dz <= 1; dz++)
	for (s16 dx = -1; dx <= 1; dx++) {
		if (!cellBlocks(snap.at(fl + v3s16(dx, 0, dz)), 1))
			continue;
		for (s16 dy = 1; dy <= 3; dy++) {
			const u8 c = snap.at(b + v3s16(dx, (s16)-dy, dz));
			total++;
			if (!(c & CELL_UNLOADED) && !(c & CELL_SKY) && cellMaterial(c) == Material::Air)
				air++;
		}
	}
	if (total == 0)
		return 0.f;
	return clampf(mat * (air / (float)total) * (thick == 1 ? 1.f : 0.5f) * 1.4f, 0.f, 1.f);
}

float terrainExcess(const HeightField &hf, v3f from, v3f to)
{
	v3f d = to - from;
	const float len = std::sqrt(d.X * d.X + d.Z * d.Z);
	if (len < 16.f)
		return 0.f;
	float worst = -1e9f;
	const float step = HeightField::CELL;
	for (float t = 8.f; t < len - 8.f; t += step) {
		const float k = t / len;
		const float x = from.X + d.X * k, z = from.Z + d.Z * k;
		const s16 h = hf.at(x, z);
		if (h == HeightField::UNKNOWN)
			continue;
		const float line = from.Y + d.Y * k;
		worst = std::max(worst, (float)h + 1.f - line);
	}
	return worst;
}

std::vector<v3f> echoPeaks(const HeightField &hf, v3f ear, v3f src, int max)
{
	struct Cand { v3f p; float score; };
	std::vector<Cand> cands;
	for (s16 j = 0; j < HeightField::N; j += 2)
	for (s16 i = 0; i < HeightField::N; i += 2) {
		const s16 h = hf.h[(size_t)j * HeightField::N + i];
		if (h == HeightField::UNKNOWN)
			continue;
		const v3f p(hf.x0 + i * HeightField::CELL + 2.f, (float)h, hf.z0 + j * HeightField::CELL + 2.f);
		const float rise = p.Y - ear.Y;
		v3f flat = p - ear;
		flat.Y = 0.f;
		const float dist = flat.getLength();
		if (rise < 12.f || dist < 48.f || dist > 190.f)
			continue;
		// a face you can see: nothing in between rises higher
		if (terrainExcess(hf, ear, p - v3f(0, 3.f, 0)) > 0.f)
			continue;
		cands.push_back({p, rise / std::sqrt(dist)});
	}
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
	std::vector<v3f> out;
	for (const Cand &c : cands) {
		v3f dir = c.p - ear;
		dir.Y = 0.f;
		dir.normalize();
		bool spread = true;
		for (const v3f &o : out) {
			v3f od = o - ear;
			od.Y = 0.f;
			od.normalize();
			if (od.dotProduct(dir) > 0.5f)
				spread = false;
		}
		if (spread)
			out.push_back(c.p);
		if ((int)out.size() >= max)
			break;
	}
	(void)src;
	return out;
}

} // namespace afx
