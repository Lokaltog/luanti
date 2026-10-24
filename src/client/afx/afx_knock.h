// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Knock to find caves: hitting a wall sounds hollow when there is open
// space behind it. Probe straight into the wall from the struck face; if
// air turns up within a few nodes, measure how much of it there is.

#pragma once

#include "irr_v3d.h"
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace afx
{

struct Knock {
	float hollow = 0.f; // 0 solid .. 1 a big space right behind a thin wall
	float pitch = 1.f;  // bigger cavities boom lower
	int depth = 0;      // wall thickness in nodes (0 = no cavity found)
	int size = 0;       // open nodes counted behind it (capped)
};

/// `open(p)`: can sound ring in node p (air, plants; not liquids or
/// unloaded)? `node` is the struck node, `into` a unit axis pointing into
/// the wall (away from the player).
template <typename F>
Knock knockProbe(F &&open, v3s16 node, v3s16 into)
{
	constexpr int MAX_DEPTH = 5, MAX_SIZE = 240, RADIUS = 10;
	Knock k;
	v3s16 start;
	for (int d = 1; d <= MAX_DEPTH; d++) {
		const v3s16 p = node + into * d;
		if (open(p)) {
			k.depth = d;
			start = p;
			break;
		}
	}
	if (k.depth == 0)
		return k;

	// How big is it? A breadth-first walk through open nodes
	static const v3s16 NB[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	auto key = [](v3s16 p) {
		return ((u64)(u16)p.X << 32) | ((u64)(u16)p.Y << 16) | (u64)(u16)p.Z;
	};
	std::unordered_set<u64> seen{key(start)};
	std::vector<v3s16> queue{start};
	const v3s16 front = node - into; // the air the player stands in
	bool same_space = false;
	for (size_t i = 0; i < queue.size() && (int)queue.size() < MAX_SIZE; i++) {
		for (const v3s16 &n : NB) {
			const v3s16 q = queue[i] + n;
			if (std::abs(q.X - start.X) > RADIUS || std::abs(q.Y - start.Y) > RADIUS ||
					std::abs(q.Z - start.Z) > RADIUS)
				continue;
			if (!seen.insert(key(q)).second || !open(q))
				continue;
			if (q == front)
				same_space = true;
			queue.push_back(q);
		}
	}
	k.size = (int)queue.size();
	static const float BY_DEPTH[MAX_DEPTH + 1] = {0.f, 1.f, 0.8f, 0.55f, 0.35f, 0.2f};
	k.hollow = std::clamp(k.size / 60.f, 0.f, 1.f) * BY_DEPTH[k.depth];
	// The air behind a pillar is the room you're in: thin, not a secret
	if (same_space)
		k.hollow *= 0.3f;
	k.pitch = std::clamp(1.25f - 0.5f * std::clamp(k.size / 200.f, 0.f, 1.f), 0.75f, 1.25f);
	return k;
}

} // namespace afx
