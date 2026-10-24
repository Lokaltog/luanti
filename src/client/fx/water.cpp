// Luanti "fx" fork: the water field (see water.h)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "water.h"
#include "light_volume.h"
#include "renderer.h"

#include "client/client.h"
#include <IVideoDriver.h>
#include "log.h"
#include "nodedef.h"
#include "itemgroup.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace fx
{

WaterField::WaterField(video::IVideoDriver *driver) : m_driver(driver) {}

WaterField::~WaterField()
{
	if (m_tex)
		m_driver->removeTexture(m_tex);
}

void WaterField::buildTables(Client *client)
{
	const NodeDefManager *ndef = client->ndef();
	m_kind.assign(ndef->size(), 0);
	for (content_t c = 0; c < ndef->size(); c++) {
		const ContentFeatures &f = ndef->get(c);
		if (f.name.empty() || f.liquid_type == LIQUID_NONE)
			continue;
		const bool lava = itemgroup_get(f.groups, "lava") > 0 || f.name.find("lava") != std::string::npos;
		if (!lava && (itemgroup_get(f.groups, "water") > 0 ||
				f.name.find("water") != std::string::npos ||
				resolveSurface(f).kind == SurfaceKind::Water))
			m_kind[c] = f.liquid_type == LIQUID_FLOWING ? 2 : 1;
	}
	m_tables = true;
}

void WaterField::build(const std::vector<u16> &snap, int sx, int sy, int sz,
		const std::vector<u8> &kind, std::vector<u8> &out)
{
	const int N = sx * sz;
	auto water = [&] (int x, int y, int z) {
		if (y < 0 || y >= sy)
			return false;
		const u16 c = snap[((size_t)z * sy + y) * sx + x];
		return c < kind.size() && kind[c] != 0;
	};
	auto flowing = [&] (int x, int y, int z) {
		const u16 c = snap[((size_t)z * sy + y) * sx + x];
		return c < kind.size() && kind[c] == 2;
	};
	// 1. per column: the highest water surface (water with no water above)
	std::vector<int> surf(N, -1), depth(N, 0);
	std::vector<u8> flow(N, 0);
	for (int z = 0; z < sz; z++)
	for (int x = 0; x < sx; x++) {
		for (int y = sy - 1; y >= 0; y--) {
			if (!water(x, y, z) || water(x, y + 1, z))
				continue;
			surf[z * sx + x] = y;
			flow[z * sx + x] = flowing(x, y, z);
			int d = 0;
			while (d < 16 && water(x, y - d - 1, z))
				d++;
			depth[z * sx + x] = d + 1;
			break;
		}
	}
	// a column is water at a level if its surface is within a node of it
	auto same_body = [&] (int a, int b) {
		return surf[a] >= 0 && surf[b] >= 0 && std::abs(surf[a] - surf[b]) <= 1;
	};

	// 2. distance to the shore: two-pass chamfer (1, sqrt 2) from the edges
	// of the water. Cells outside the window count as water (no false shore
	// along the window edge).
	const float INF = 1e9f, CAP = 32.f;
	std::vector<float> dist(N, INF);
	static const int NX[8] = {-1, 1, 0, 0, -1, 1, -1, 1}, NZ[8] = {0, 0, -1, 1, -1, -1, 1, 1};
	for (int z = 0; z < sz; z++)
	for (int x = 0; x < sx; x++) {
		const int i = z * sx + x;
		if (surf[i] < 0)
			continue;
		// a water cell next to land: the shore is half a node away
		for (int k = 0; k < 4; k++) {
			const int nx = x + NX[k], nz = z + NZ[k];
			if (nx < 0 || nz < 0 || nx >= sx || nz >= sz)
				continue;
			if (!same_body(i, nz * sx + nx)) {
				dist[i] = 0.5f;
				break;
			}
		}
	}
	const float D = 1.f, DD = 1.41421f;
	for (int z = 0; z < sz; z++)
	for (int x = 0; x < sx; x++) {
		const int i = z * sx + x;
		if (surf[i] < 0)
			continue;
		float d = dist[i];
		if (x > 0 && surf[i - 1] >= 0) d = std::min(d, dist[i - 1] + D);
		if (z > 0 && surf[i - sx] >= 0) d = std::min(d, dist[i - sx] + D);
		if (x > 0 && z > 0 && surf[i - sx - 1] >= 0) d = std::min(d, dist[i - sx - 1] + DD);
		if (x < sx - 1 && z > 0 && surf[i - sx + 1] >= 0) d = std::min(d, dist[i - sx + 1] + DD);
		dist[i] = d;
	}
	for (int z = sz - 1; z >= 0; z--)
	for (int x = sx - 1; x >= 0; x--) {
		const int i = z * sx + x;
		if (surf[i] < 0)
			continue;
		float d = dist[i];
		if (x < sx - 1 && surf[i + 1] >= 0) d = std::min(d, dist[i + 1] + D);
		if (z < sz - 1 && surf[i + sx] >= 0) d = std::min(d, dist[i + sx] + D);
		if (x < sx - 1 && z < sz - 1 && surf[i + sx + 1] >= 0) d = std::min(d, dist[i + sx + 1] + DD);
		if (x > 0 && z < sz - 1 && surf[i + sx - 1] >= 0) d = std::min(d, dist[i + sx - 1] + DD);
		dist[i] = d;
	}

	// 3. openness: the largest shore distance in each body of water; a body
	// that runs out of the window is at least a lake
	std::vector<int> label(N, -1);
	std::vector<float> open;
	std::deque<int> q;
	for (int s = 0; s < N; s++) {
		if (surf[s] < 0 || label[s] >= 0)
			continue;
		const int id = (int)open.size();
		float best = 0.f;
		bool edge = false;
		label[s] = id;
		q.push_back(s);
		while (!q.empty()) {
			const int i = q.front();
			q.pop_front();
			best = std::max(best, std::min(dist[i], CAP));
			const int x = i % sx, z = i / sx;
			if (x == 0 || z == 0 || x == sx - 1 || z == sz - 1)
				edge = true;
			for (int k = 0; k < 4; k++) {
				const int nx = x + NX[k], nz = z + NZ[k];
				if (nx < 0 || nz < 0 || nx >= sx || nz >= sz)
					continue;
				const int j = nz * sx + nx;
				if (label[j] < 0 && same_body(i, j)) {
					label[j] = id;
					q.push_back(j);
				}
			}
		}
		open.push_back(edge ? std::max(best, 12.f) : best);
	}

	out.assign((size_t)N * 4, 0);
	u32 *px = reinterpret_cast<u32 *>(out.data());
	for (int i = 0; i < N; i++) {
		if (surf[i] < 0) {
			px[i] = video::SColor(0, 0, 0, 0).color;
			continue;
		}
		const float d = std::min(dist[i] >= INF ? CAP : dist[i], CAP);
		const u32 r = (u32)std::lround(d / CAP * 255.f);
		const u32 g = (u32)std::lround(std::min(open[label[i]], CAP) / CAP * 255.f);
		const u32 b = (u32)std::lround(std::min(depth[i], 16) / 16.f * 127.f) + (flow[i] ? 128 : 0);
		const u32 a = (u32)std::min(255, (int)std::lround((surf[i] + 1) / 64.f * 255.f));
		px[i] = video::SColor(a, r, g, b).color;
	}
}

void WaterField::update(Client *client, const LightVolume &vol, float dtime)
{
	m_timer += dtime;
	if (!vol.hasSnapshot())
		return;
	const u32 gen = vol.snapshotGeneration();
	if ((gen == m_generation && m_valid) || (m_timer < 0.25f && m_valid))
		return;
	if (!m_tables)
		buildTables(client);
	m_timer = 0.f;
	m_generation = gen;
	static_assert(LightVolume::SX == SIZE && LightVolume::SZ == SIZE, "water field = volume footprint");
	build(vol.snapshot(), LightVolume::SX, LightVolume::SY, LightVolume::SZ, m_kind, m_px);

	if (!m_tex) {
		const bool mip = m_driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS);
		m_driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, false);
		m_tex = m_driver->addTexture(core::dimension2du(SIZE, SIZE), "fx_water_field",
				video::ECF_A8R8G8B8);
		m_driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, mip);
		if (!m_tex) {
			warningstream << "fx: could not create the water field texture" << std::endl;
			return;
		}
	}
	void *dst = m_tex->lock(video::ETLM_WRITE_ONLY);
	if (!dst)
		return;
	const u32 pitch = m_tex->getPitch();
	for (u32 row = 0; row < (u32)SIZE; row++)
		std::memcpy((u8 *)dst + row * pitch, m_px.data() + (size_t)row * SIZE * 4, SIZE * 4);
	m_tex->unlock();
	m_origin = vol.snapshotOrigin();
	m_valid = true;
	u32 cols = 0, shore = 0;
	for (int i = 0; i < SIZE * SIZE; i++) {
		cols += m_px[i * 4 + 3] > 0;
		shore += m_px[i * 4 + 3] > 0 && m_px[i * 4 + 2] < 8; // SColor in memory: B G R A
	}
	verbosestream << "fx: water field: " << cols << " water columns, " << shore
		<< " at the shore, origin " << m_origin.X << "," << m_origin.Y << "," << m_origin.Z << std::endl;
}

} // namespace fx
