// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Synthetic voxel worlds for the probe tests and the listening renders.

#pragma once

#include "client/afx/afx_types.h"
#include <algorithm>

namespace afx_test {

using namespace afx;

struct World {
	VoxelSnapshot snap;

	explicit World(s16 size = 64)
	{
		snap.size = size;
		snap.origin = v3s16(-size / 2, -size / 2, -size / 2);
		snap.cells.assign((size_t)size * size * size, (u8)Material::Air);
	}

	void fill(v3s16 a, v3s16 b, Material m)
	{
		for (s16 z = std::min(a.Z, b.Z); z <= std::max(a.Z, b.Z); z++)
		for (s16 y = std::min(a.Y, b.Y); y <= std::max(a.Y, b.Y); y++)
		for (s16 x = std::min(a.X, b.X); x <= std::max(a.X, b.X); x++) {
			v3s16 p(x, y, z);
			if (snap.contains(p))
				snap.cells[snap.index(p)] = (u8)m;
		}
	}

	/// Sunlight: air with nothing solid above it, like the engine's day bank.
	void markSky()
	{
		const s16 s = snap.size;
		for (s16 z = 0; z < s; z++)
		for (s16 x = 0; x < s; x++) {
			for (s16 y = s - 1; y >= 0; y--) {
				v3s16 p = snap.origin + v3s16(x, y, z);
				u8 &c = snap.cells[snap.index(p)];
				if (cellMaterial(c) != Material::Air)
					break;
				c |= CELL_SKY;
			}
		}
	}

	Acoustics probe(v3f ear) const { return analyze(snap, ear, Params()); }
};


inline World openField()
{
	World w;
	w.fill({-32, -32, -32}, {31, -1, 31}, Material::Earth);
	w.markSky();
	return w;
}

inline World stoneCave()
{
	// A 13x7x13 hollow deep inside stone, sky far above.
	World w;
	w.fill({-32, -32, -32}, {31, 20, 31}, Material::Stone);
	w.fill({-6, -3, -6}, {6, 3, 6}, Material::Air);
	w.markSky();
	return w;
}

} // namespace afx_test
