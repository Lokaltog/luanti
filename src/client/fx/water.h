// Luanti "fx" fork: what the water around the camera looks like as bodies
// of water: distance to the shore, how open the water is, how deep
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Built on the main thread from the light volume's node snapshot (96x48x96
// around the camera) whenever it changes, at most 4 times a second, into a
// 96x96 RGBA8 texture, one texel per column:
//   R  distance to the shore along the surface, nodes / 32 (bilinear: smooth)
//   G  openness: the largest shore distance in this body of water, nodes / 32
//      (a pond is ~0.05, a lake ~0.3, the sea 1); waves scale with it
//   B  depth below the surface, nodes / 16
//   A  surface height in the window + 1, / 64 (0 = no water in this column)
// The water shaders sample it to size waves, turn them towards the shore and
// break them into foam there.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <vector>

class Client;
namespace video { class IVideoDriver; class ITexture; }

namespace fx
{

class LightVolume;

class WaterField
{
public:
	explicit WaterField(video::IVideoDriver *driver);
	~WaterField();

	void update(Client *client, const LightVolume &vol, float dtime);

	video::ITexture *texture() const { return m_valid ? m_tex : nullptr; }
	bool valid() const { return m_valid; }
	/// world node position of texel (0, 0) (y: of window layer 0)
	v3s16 origin() const { return m_origin; }

	static constexpr int SIZE = 96;

	/// CPU-side build, exposed for the unit test: content ids in the light
	/// volume snapshot layout, is_water per content id. Output RGBA8 texels.
	static void build(const std::vector<u16> &snap, int sx, int sy, int sz,
			const std::vector<bool> &is_water, std::vector<u8> &out);

private:
	void buildTables(Client *client);

	video::IVideoDriver *m_driver;
	video::ITexture *m_tex = nullptr;
	std::vector<bool> m_is_water;
	std::vector<u8> m_px;
	v3s16 m_origin;
	u32 m_generation = 0;
	float m_timer = 1.f;
	bool m_valid = false;
	bool m_tables = false;
};

} // namespace fx
