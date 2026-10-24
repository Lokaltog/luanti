// Luanti "fx" fork: M4 RGB light volume
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "light_volume.h"
#include "fx.h"
#include "renderer.h"

#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientmap.h"
#include "client/mapblock_mesh.h"
#include "client/node_visuals.h"
#include "client/texturesource.h"
#include "client/renderingengine.h"
#include "log.h"
#include "mapblock.h"
#include "nodedef.h"
#include "porting.h"
#include <IVideoDriver.h>
#include <ITexture.h>
#include <algorithm>
#include <cstring>

namespace fx
{

static constexpr size_t VOXELS = (size_t)LightVolume::SX * LightVolume::SY * LightVolume::SZ;

static inline size_t vidx(s16 x, s16 y, s16 z)
{
	return ((size_t)z * LightVolume::SY + y) * LightVolume::SX + x;
}

LightVolume::LightVolume(video::IVideoDriver *driver) : m_driver(driver)
{
	m_thread = std::thread([this] { workerLoop(); });
}

LightVolume::~LightVolume()
{
	{
		std::lock_guard lock(m_mutex);
		m_quit = true;
	}
	m_cv.notify_all();
	if (m_thread.joinable())
		m_thread.join();
	// the driver outlives worlds: free the texture with its world
	if (m_texture)
		m_driver->removeTexture(m_texture);
}

void LightVolume::buildTables(Client *client)
{
	const NodeDefManager *ndef = client->ndef();
	ITextureSource *tsrc = client->getTextureSource();
	m_info.assign(ndef->size(), ContentInfo());
	for (content_t c = 0; c < ndef->size(); c++) {
		const ContentFeatures &f = ndef->get(c);
		ContentInfo &ci = m_info[c];
		ci.opaque = !f.light_propagates;
		if (c == CONTENT_AIR)
			ci.opaque = false;
		if (f.light_source > 0 && f.visuals) {
			const video::SColor col = f.visuals->fx_light.color; // hue, max channel 255
			const u8 lvl = std::min<u8>(f.light_source, 14);
			ci.src[0] = (u8)((lvl * col.getRed() + 127) / 255);
			ci.src[1] = (u8)((lvl * col.getGreen() + 127) / 255);
			ci.src[2] = (u8)((lvl * col.getBlue() + 127) / 255);
		}
		// Glass transmits its colour. Clear glass (low saturation) stays white.
		if (!ci.opaque && detectSurface(f).kind == SurfaceKind::Glass && !f.tiledef[0].name.empty()) {
			video::SColor avg = tsrc->getTextureAverageColor(f.tiledef[0].name);
			float r = avg.getRed(), g = avg.getGreen(), b = avg.getBlue();
			float mx = std::max({r, g, b, 1.f}), mn = std::min({r, g, b});
			float sat = (mx - mn) / mx;
			float k = std::clamp(sat * 1.5f, 0.f, 1.f) * 0.9f;
			const float ch[3] = {r / mx, g / mx, b / mx};
			for (int i = 0; i < 3; i++)
				ci.tint[i] = (u8)std::clamp(255.f * (1.f + (ch[i] - 1.f) * k), 0.f, 255.f);
		}
	}
	m_tables_built = true;
	infostream << "fx: light volume tables built for " << ndef->size() << " contents" << std::endl;
}

bool LightVolume::readBox(Client *client, v3s16 lo, v3s16 hi)
{
	const v3s16 last = m_origin + v3s16(SX - 1, SY - 1, SZ - 1);
	lo = v3s16(std::max(lo.X, m_origin.X), std::max(lo.Y, m_origin.Y), std::max(lo.Z, m_origin.Z));
	hi = v3s16(std::min(hi.X, last.X), std::min(hi.Y, last.Y), std::min(hi.Z, last.Z));
	if (lo.X > hi.X || lo.Y > hi.Y || lo.Z > hi.Z)
		return false;
	ClientMap &map = client->getEnv().getClientMap();
	const v3s16 b0 = getNodeBlockPos(lo), b1 = getNodeBlockPos(hi);
	v3s16 bp;
	for (bp.Z = b0.Z; bp.Z <= b1.Z; bp.Z++)
	for (bp.Y = b0.Y; bp.Y <= b1.Y; bp.Y++)
	for (bp.X = b0.X; bp.X <= b1.X; bp.X++) {
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		const v3s16 base = bp * MAP_BLOCKSIZE;
		const v3s16 l(std::max(base.X, lo.X), std::max(base.Y, lo.Y), std::max(base.Z, lo.Z));
		const v3s16 h(std::min<s16>(base.X + MAP_BLOCKSIZE - 1, hi.X),
				std::min<s16>(base.Y + MAP_BLOCKSIZE - 1, hi.Y),
				std::min<s16>(base.Z + MAP_BLOCKSIZE - 1, hi.Z));
		for (s16 z = l.Z; z <= h.Z; z++)
		for (s16 y = l.Y; y <= h.Y; y++) {
			const size_t row = vidx(0, y - m_origin.Y, z - m_origin.Z);
			for (s16 x = l.X; x <= h.X; x++)
				m_content[row + (x - m_origin.X)] = block ? block->getNodeNoCheck(x - base.X, y - base.Y, z - base.Z).getContent()
						: CONTENT_IGNORE;
		}
	}
	return true;
}

void LightVolume::moveTo(Client *client, v3s16 origin)
{
	const v3s16 size(SX, SY, SZ);
	const v3s16 d = origin - m_origin;
	std::vector<u16> old;
	old.swap(m_content);
	m_content.assign(VOXELS, CONTENT_IGNORE);
	m_origin = origin;
	// the overlap of the old and new windows, in new-window coordinates
	v3s16 lo, hi;
	bool overlap = true;
	for (int a = 0; a < 3; a++) {
		const s16 da = a == 0 ? d.X : a == 1 ? d.Y : d.Z;
		const s16 n = a == 0 ? size.X : a == 1 ? size.Y : size.Z;
		const s16 l = std::max<s16>(0, -da), h = std::min<s16>(n - 1, n - 1 - da);
		overlap &= l <= h;
		(a == 0 ? lo.X : a == 1 ? lo.Y : lo.Z) = l;
		(a == 0 ? hi.X : a == 1 ? hi.Y : hi.Z) = h;
	}
	if (!overlap) {
		readBox(client, origin, origin + size - v3s16(1, 1, 1));
		return;
	}
	for (s16 z = lo.Z; z <= hi.Z; z++)
	for (s16 y = lo.Y; y <= hi.Y; y++)
		std::memcpy(&m_content[vidx(lo.X, y, z)], &old[vidx(lo.X + d.X, y + d.Y, z + d.Z)],
				(hi.X - lo.X + 1) * sizeof(u16));
	// read the newly exposed slabs: X outside the overlap, then Y within
	// its X range, then Z within its X and Y range
	const v3s16 o = origin, e = origin + size - v3s16(1, 1, 1);
	const v3s16 olo = origin + lo, ohi = origin + hi;
	if (olo.X > o.X) readBox(client, o, v3s16(olo.X - 1, e.Y, e.Z));
	if (ohi.X < e.X) readBox(client, v3s16(ohi.X + 1, o.Y, o.Z), e);
	if (olo.Y > o.Y) readBox(client, v3s16(olo.X, o.Y, o.Z), v3s16(ohi.X, olo.Y - 1, e.Z));
	if (ohi.Y < e.Y) readBox(client, v3s16(olo.X, ohi.Y + 1, o.Z), v3s16(ohi.X, e.Y, e.Z));
	if (olo.Z > o.Z) readBox(client, v3s16(olo.X, olo.Y, o.Z), v3s16(ohi.X, ohi.Y, olo.Z - 1));
	if (ohi.Z < e.Z) readBox(client, v3s16(olo.X, olo.Y, ohi.Z + 1), v3s16(ohi.X, ohi.Y, e.Z));
}

void floodFillRGB(s16 sx, s16 sy, s16 sz, const u16 *content,
		const std::vector<LightCell> &cells, std::vector<u8> &lvl)
{
	const size_t voxels = (size_t)sx * sy * sz;
	lvl.assign(voxels * 3, 0);
	std::vector<u32> queue;
	queue.reserve(8192);
	static const LightCell opaque_unknown;
	auto info = [&] (u16 c) -> const LightCell & {
		return c < cells.size() ? cells[c] : opaque_unknown;
	};
	auto idx = [&] (s16 x, s16 y, s16 z) { return ((size_t)z * sy + y) * sx + x; };

	for (size_t i = 0; i < voxels; i++) {
		const LightCell &ci = info(content[i]);
		if (ci.src[0] | ci.src[1] | ci.src[2]) {
			lvl[i * 3 + 0] = ci.src[0];
			lvl[i * 3 + 1] = ci.src[1];
			lvl[i * 3 + 2] = ci.src[2];
			queue.push_back((u32)i);
		}
	}

	static const s16 dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	for (size_t head = 0; head < queue.size(); head++) {
		const u32 i = queue[head];
		const s16 x = i % sx, y = (i / sx) % sy, z = i / ((u32)sx * sy);
		const u8 l[3] = {lvl[i * 3], lvl[i * 3 + 1], lvl[i * 3 + 2]};
		if (l[0] <= 1 && l[1] <= 1 && l[2] <= 1)
			continue;
		for (const auto &d : dirs) {
			const s16 nx = x + d[0], ny = y + d[1], nz = z + d[2];
			if (nx < 0 || ny < 0 || nz < 0 || nx >= sx || ny >= sy || nz >= sz)
				continue;
			const size_t j = idx(nx, ny, nz);
			const LightCell &cj = info(content[j]);
			if (cj.opaque)
				continue;
			bool improved = false;
			for (int c = 0; c < 3; c++) {
				int v = (int)l[c] - 1;
				if (v <= 0)
					continue;
				if (cj.tint[c] != 255)
					v = v * cj.tint[c] / 255;
				if (v > lvl[j * 3 + c]) {
					lvl[j * 3 + c] = (u8)v;
					improved = true;
				}
			}
			if (improved)
				queue.push_back((u32)j);
		}
	}
}

void LightVolume::compute(const Job &job, std::vector<u8> &out)
{
	std::vector<u8> lvl;
	floodFillRGB(SX, SY, SZ, job.content.data(), m_info, lvl);

	// Pack into the atlas: Y slices tiled ATLAS_COLS per row, ARGB8 texels.
	out.assign((size_t)ATLAS_W * ATLAS_H * 4, 0);
	u32 *px = reinterpret_cast<u32 *>(out.data());
	for (s16 y = 0; y < SY; y++) {
		const u32 tx = (y % ATLAS_COLS) * SX, ty = (y / ATLAS_COLS) * SZ;
		for (s16 z = 0; z < SZ; z++)
		for (s16 x = 0; x < SX; x++) {
			const u8 *l = &lvl[vidx(x, y, z) * 3];
			const u32 r = l[0] * 17, g = l[1] * 17, b = l[2] * 17;
			const u32 a = std::max({r, g, b});
			px[(ty + z) * ATLAS_W + tx + x] = video::SColor(a, r, g, b).color;
		}
	}
}

void LightVolume::workerLoop()
{
	for (;;) {
		std::unique_ptr<Job> job;
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait(lock, [this] { return m_quit || m_pending; });
			if (m_quit)
				return;
			job = std::move(m_pending);
		}
		std::vector<u8> result;
		const u64 t0 = porting::getTimeMs();
		compute(*job, result);
		const u64 t1 = porting::getTimeMs();
		{
			std::lock_guard lock(m_mutex);
			m_result = std::move(result);
			m_result_origin = job->origin;
			m_result_generation = job->generation;
			m_result_ready = true;
		}
		m_busy = false;
		verbosestream << "fx: light volume computed in " << (t1 - t0) << " ms" << std::endl;
	}
}

void LightVolume::upload(video::IVideoDriver *driver)
{
	if (!m_texture) {
		const bool mip = driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS);
		driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, false);
		m_texture = driver->addTexture(core::dimension2du(ATLAS_W, ATLAS_H),
				"fx_light_volume", video::ECF_A8R8G8B8);
		driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, mip);
		if (!m_texture) {
			warningstream << "fx: could not create the light volume texture" << std::endl;
			return;
		}
	}
	void *dst = m_texture->lock(video::ETLM_WRITE_ONLY);
	if (!dst)
		return;
	const u32 pitch = m_texture->getPitch();
	for (u32 row = 0; row < ATLAS_H; row++)
		std::memcpy((u8 *)dst + row * pitch, m_result.data() + (size_t)row * ATLAS_W * 4, ATLAS_W * 4);
	m_texture->unlock();
	m_uploaded.swap(m_result);
	m_uploaded_origin = m_result_origin;
	m_uploaded_generation = m_result_generation;
	m_have_result = true;
}

bool LightVolume::sampleLevels(v3s16 node, u8 out[3]) const
{
	if (!m_have_result || m_uploaded.empty())
		return false;
	const v3s16 v = node - m_uploaded_origin;
	if (v.X < 0 || v.Y < 0 || v.Z < 0 || v.X >= SX || v.Y >= SY || v.Z >= SZ)
		return false;
	const u32 tx = (v.Y % ATLAS_COLS) * SX + v.X, ty = (v.Y / ATLAS_COLS) * SZ + v.Z;
	const video::SColor c(reinterpret_cast<const u32 *>(m_uploaded.data())[ty * ATLAS_W + tx]);
	out[0] = c.getRed() / 17;
	out[1] = c.getGreen() / 17;
	out[2] = c.getBlue() / 17;
	return true;
}

void LightVolume::update(Client *client, v3s16 camera_node, float dtime)
{
	if (!m_tables_built)
		buildTables(client);

	// 1. upload a finished result
	{
		std::lock_guard lock(m_mutex);
		if (m_result_ready) {
			m_result_ready = false;
			upload(m_driver);
		}
	}

	// 2. follow the camera: re-centre when it is 8 nodes (6 vertically) off
	// centre, origin snapped to 4 nodes
	const v3s16 half(SX / 2, SY / 2, SZ / 2);
	v3s16 want = camera_node - half;
	want = v3s16(want.X & ~3, want.Y & ~3, want.Z & ~3);
	if (!m_have_content) {
		m_origin = want;
		m_content.assign(VOXELS, CONTENT_IGNORE);
		readBox(client, m_origin, m_origin + v3s16(SX - 1, SY - 1, SZ - 1));
		m_have_content = true;
		m_dirty.clear();
		m_generation++;
	} else {
		const v3s16 c = m_origin + half;
		if (std::abs(camera_node.X - c.X) > 8 || std::abs(camera_node.Y - c.Y) > 6 ||
				std::abs(camera_node.Z - c.Z) > 8) {
			moveTo(client, want);
			m_generation++;
		}
	}

	// 3. re-read blocks the client changed
	if (!m_dirty.empty()) {
		bool changed = false;
		for (const v3s16 bp : m_dirty) {
			const v3s16 base = bp * MAP_BLOCKSIZE;
			changed |= readBox(client, base, base + v3s16(1, 1, 1) * (MAP_BLOCKSIZE - 1));
		}
		m_dirty.clear();
		if (changed)
			m_generation++;
	}

	// 4. hand a changed snapshot to the worker (never two at once, at most
	// every 0.1 s while things keep changing)
	m_timer += dtime;
	if (m_generation == m_job_generation || m_busy || m_timer < 0.1f)
		return;
	m_timer = 0.f;
	auto job = std::make_unique<Job>();
	job->origin = m_origin;
	job->generation = m_generation;
	job->content = m_content;
	m_job_generation = m_generation;
	m_busy = true;
	{
		std::lock_guard lock(m_mutex);
		m_pending = std::move(job);
	}
	m_cv.notify_one();
}

} // namespace fx
