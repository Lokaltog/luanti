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

static const MaterialProps MATERIALS[MATERIAL_COUNT] = {
	//  name       absorb  hf    pass  pass_hf
	{"air",        0.00f, 1.00f,  1.0f, 0.035f},
	{"leaky",      0.30f, 0.80f,  3.0f, 0.40f},
	{"foliage",    0.50f, 0.35f,  2.5f, 0.35f},
	{"soft",       0.70f, 0.20f, 12.0f, 2.50f},
	{"snow",       0.60f, 0.30f, 14.0f, 2.50f},
	{"glass",      0.04f, 0.95f,  8.0f, 1.60f},
	{"wood",       0.25f, 0.60f, 16.0f, 2.00f},
	{"earth",      0.12f, 0.60f, 24.0f, 2.50f},
	{"stone",      0.07f, 0.90f, 30.0f, 3.00f},
	{"water",      0.03f, 0.90f,  6.0f, 1.50f},
	{"lava",       0.10f, 0.60f, 30.0f, 3.00f},
};

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
	coordinates, so voxel boundaries sit at .5. Calls `visit(cell_pos, t)`
	for every voxel entered after the starting one, where t is the distance
	travelled; stops when visit returns false or t exceeds max_t.
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
		if (!visit(cell, t))
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
	traverse(ear, dir, max_t, [&](v3s16 p, float t) {
		u8 cell = snap.at(p);
		if (cell & CELL_UNLOADED)
			return false; // out of the snapshot: treat as open
		Material m = cellMaterial(cell);
		if (m == Material::Air) {
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
	Outdoor path: Dial's algorithm (bucket-queue Dijkstra) over the snapshot.
	Costs are in quarter nodes so integer buckets stay exact enough.
*/

namespace {

struct PathResult {
	bool found = false;
	float cost = 0.f;   // nodes of air-equivalent
	float hf_loss = 0.f;// nepers
	v3f dir;            // from ear toward the opening along the path
	float steps = 0.f;  // path length in voxels
};

struct PathScratch {
	std::vector<u16> dist;
	std::vector<u8> parent;     // 0 = unvisited, 1..6 = came from axis dir, 7 = start
	std::vector<float> hf;
	std::vector<std::vector<u32>> buckets;
};

} // namespace

static constexpr int COST_SCALE = 4;
static constexpr int PATH_LIMIT_NODES = 64;
static constexpr u32 PATH_POP_BUDGET = 120000;

static const v3s16 NEIGHBORS[6] = {
	{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

static PathResult findSky(const VoxelSnapshot &snap, v3s16 start, v3f ear)
{
	PathResult res;
	if (!snap.contains(start))
		return res;

	thread_local PathScratch s;
	const size_t n = snap.cells.size();
	s.dist.assign(n, 0xffff);
	s.parent.assign(n, 0);
	s.hf.assign(n, 0.f);

	int max_edge = 0;
	for (const auto &m : MATERIALS)
		max_edge = std::max(max_edge, (int)std::lround(m.pass * COST_SCALE));
	const int nb = max_edge + 1;
	s.buckets.resize(nb);
	for (auto &b : s.buckets)
		b.clear();

	const int limit = PATH_LIMIT_NODES * COST_SCALE;
	u32 si = snap.index(start);
	s.dist[si] = 0;
	s.parent[si] = 7;
	s.buckets[0].push_back(si);
	size_t pending = 1;
	u32 pops = 0;
	u32 found_idx = U32_MAX;

	auto pos_of = [&](u32 i) {
		s16 x = i % snap.size, y = (i / snap.size) % snap.size, z = i / (snap.size * snap.size);
		return snap.origin + v3s16(x, y, z);
	};

	for (int cost = 0; pending > 0 && cost <= limit && found_idx == U32_MAX; cost++) {
		auto &bucket = s.buckets[cost % nb];
		while (!bucket.empty()) {
			u32 i = bucket.back();
			bucket.pop_back();
			pending--;
			if (s.dist[i] != cost)
				continue; // stale entry
			if (++pops > PATH_POP_BUDGET) {
				pending = 0;
				break;
			}
			if (cellSky(snap.cells[i])) {
				found_idx = i;
				break;
			}
			v3s16 p = pos_of(i);
			for (int k = 0; k < 6; k++) {
				v3s16 q = p + NEIGHBORS[k];
				if (!snap.contains(q))
					continue;
				u32 qi = snap.index(q);
				u8 cell = snap.cells[qi];
				const MaterialProps &mp = props(cellMaterial(cell));
				int nc = cost + (int)std::lround(mp.pass * COST_SCALE);
				if (nc > limit || nc >= s.dist[qi])
					continue;
				s.dist[qi] = nc;
				s.parent[qi] = k + 1;
				s.hf[qi] = s.hf[i] + mp.pass_hf;
				s.buckets[nc % nb].push_back(qi);
				pending++;
			}
		}
	}

	if (found_idx == U32_MAX)
		return res;

	res.found = true;
	res.cost = s.dist[found_idx] / (float)COST_SCALE;
	res.hf_loss = s.hf[found_idx];

	// Walk back to the ear. The direction sound arrives from is where the path
	// is a few nodes out, which points at the doorway rather than the sky.
	std::vector<v3s16> path;
	u32 i = found_idx;
	while (s.parent[i] != 7 && path.size() < 4096) {
		v3s16 p = pos_of(i);
		path.push_back(p);
		p -= NEIGHBORS[s.parent[i] - 1];
		i = snap.index(p);
	}
	res.steps = (float)path.size();
	if (!path.empty()) {
		// path is target..first step; pick the point ~5 voxels from the ear
		size_t k = path.size() > 5 ? path.size() - 5 : 0;
		v3f to = intToFloat(path[k], 1.f);
		v3f d = to - ear;
		if (d.getLengthSQ() > 1e-4f)
			res.dir = d.normalize();
	}
	return res;
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

Acoustics analyze(const VoxelSnapshot &snap, v3f ear, const Params &params)
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
	out.hf = hit_hf_avg;

	// Sabine: RT60 = 0.161 V / (S a). Treating the rays as samples of a
	// star-shaped room, V/S = E[d³] / (3 E[d²]).
	// Sabine: RT60 = 0.161 V / (S a), with V/S from the mean hit distance
	// (a sphere seen from its centre has V/S = r/3). The mean is robust where
	// a cubic moment is not: one long ray down a corridor doesn't make the
	// corridor a cathedral. Escaped rays only add absorption.
	const float v_over_s = (hits > 0 ? hit_d / hits : max_t) / 3.f;
	const float rt60 = 0.161f * v_over_s / avg_abs;
	// Open land still has a faint, long, distant tail (the "big outdoors").
	const float open_tail = lerpf(1.8f, 0.f, out.enclosure);
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
	const float liveness = std::pow(1.f - hit_abs_avg, 3.f);
	out.reflections_gain = clampf((0.05f + out.enclosure * (1.f - hit_abs_avg)) *
			clampf(6.f / std::max(1.f, hit_d_avg), 0.2f, 1.5f), 0.f, 3.16f);
	out.late_gain = clampf(1.6f * (1.f - hit_abs_avg), 0.2f, 3.f);
	out.wet = clampf(params.reverb * lerpf(0.3f, 1.f, out.enclosure) * liveness, 0.f, 1.f);
	if (pan_w > 0) {
		v3f p = pan / pan_w;
		out.reflections_pan = p * std::min(1.f, 1.f - out.enclosure * 0.5f);
	}

	// Echo
	EchoCluster echo = findEcho(rays);
	if (echo.weight > 0) {
		float strength = clampf((echo.weight - 0.02f) / 0.08f, 0.f, 1.f) *
				clampf(25.f / echo.dist, 0.4f, 1.f);
		out.echo = clampf(strength * params.echo, 0.f, 1.f);
		out.echo_delay = clampf(2.f * echo.dist / SPEED_OF_SOUND, 0.02f, 0.207f);
		out.echo_damping = clampf(1.f - echo.hf, 0.f, 0.99f);
		out.echo_dir = echo.dir;
		EchoCluster back = findEcho(rays, echo.dir, true);
		out.echo_feedback = back.weight > 0.04f ? 0.35f : 0.1f;
	}

	// Outdoors
	if (out.underwater || out.in_lava) {
		out.outdoor_gain = 0.15f;
		out.outdoor_hf = 0.05f;
	} else if (cellSky(snap.at(ear_cell))) {
		out.outdoor_gain = 1.f;
		out.outdoor_hf = 1.f;
		out.outdoor_path = 0.f;
		out.outdoor_focus = 0.f;
	} else {
		PathResult path = findSky(snap, ear_cell, ear);
		if (path.found) {
			const float k = std::max(0.f, params.outdoor_muffle);
			out.outdoor_path = path.cost;
			out.outdoor_gain = std::exp(-path.cost * k / 14.f);
			out.outdoor_hf = std::exp(-path.hf_loss * k);
			out.outdoor_dir = path.dir;
			const float sky_frac = sky / n;
			out.outdoor_focus = clampf(1.f - 2.f * sky_frac, 0.f, 1.f) *
					clampf(path.steps / 4.f, 0.f, 1.f);
		} else {
			out.outdoor_gain = 0.f;
			out.outdoor_hf = 0.f;
			out.outdoor_path = (float)PATH_LIMIT_NODES;
			out.outdoor_focus = 1.f;
		}
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
	traverse(from, d, len, [&](v3s16 p, float) {
		if (p == end)
			return false; // the emitter's own node doesn't block it
		u8 cell = snap.at(p);
		Material m = cellMaterial(cell);
		if (m != Material::Air) {
			cost += props(m).pass;
			hf_loss += props(m).pass_hf;
		}
		return cost < 120.f;
	});
	return cost;
}

Occlusion occlusion(const VoxelSnapshot &snap, v3f from, v3f to)
{
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

	int order[5] = {0, 1, 2, 3, 4};
	std::sort(order, order + 5, [&](int a, int b) { return costs[a] < costs[b]; });
	// The direct ray counts double so a clear line of sight always wins.
	float cost = 0.5f * (std::min(costs[0], costs[order[0]]) + costs[order[1]]);
	float hf = 0.5f * (hfs[order[0]] + hfs[order[1]]);
	if (costs[0] <= 0.f) {
		cost = 0.f;
		hf = 0.f;
	}

	Occlusion o;
	o.amount = 1.f - std::exp(-cost / 6.f);
	o.hf = std::exp(-hf);
	return o;
}

} // namespace afx
