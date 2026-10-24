// Luanti "fx" fork: M4 RGB light volume
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "light_volume.h"
#include "fx.h"

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

LightVolume::LightVolume()
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
	// the texture belongs to the driver's cache and is dropped with it
}

void LightVolume::reset()
{
	std::lock_guard lock(m_mutex);
	m_tables_built = false;
	m_have_result = false;
	m_result_ready = false;
	m_job_origin = v3s16(-32768, -32768, -32768);
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
		if (!ci.opaque && resolveSurface(f).kind == SurfaceKind::Glass && !f.tiledef[0].name.empty()) {
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

// Cheap change detection: block and mesh pointers in the region. The mesh
// pointer changes whenever a block is re-meshed (edited, lit, loaded).
u64 LightVolume::signature(Client *client, v3s16 origin)
{
	ClientMap &map = client->getEnv().getClientMap();
	const v3s16 b0 = getNodeBlockPos(origin);
	const v3s16 b1 = getNodeBlockPos(origin + v3s16(SX - 1, SY - 1, SZ - 1));
	u64 sig = 1469598103934665603ULL;
	v3s16 bp;
	for (bp.Z = b0.Z; bp.Z <= b1.Z; bp.Z++)
	for (bp.Y = b0.Y; bp.Y <= b1.Y; bp.Y++)
	for (bp.X = b0.X; bp.X <= b1.X; bp.X++) {
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		sig ^= (u64)(uintptr_t)block + 0x9e3779b97f4a7c15ULL + (sig << 6) + (sig >> 2);
		sig ^= (u64)(uintptr_t)(block ? block->mesh : nullptr) + (sig << 6) + (sig >> 2);
	}
	return sig;
}

void LightVolume::snapshot(Client *client, v3s16 origin, Job &job)
{
	ClientMap &map = client->getEnv().getClientMap();
	job.origin = origin;
	job.content.assign(VOXELS, CONTENT_IGNORE);
	const v3s16 last = origin + v3s16(SX - 1, SY - 1, SZ - 1);
	const v3s16 b0 = getNodeBlockPos(origin), b1 = getNodeBlockPos(last);
	v3s16 bp;
	for (bp.Z = b0.Z; bp.Z <= b1.Z; bp.Z++)
	for (bp.Y = b0.Y; bp.Y <= b1.Y; bp.Y++)
	for (bp.X = b0.X; bp.X <= b1.X; bp.X++) {
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		if (!block)
			continue;
		const v3s16 base = bp * MAP_BLOCKSIZE;
		const v3s16 lo(std::max(base.X, origin.X), std::max(base.Y, origin.Y), std::max(base.Z, origin.Z));
		const v3s16 hi(std::min<s16>(base.X + MAP_BLOCKSIZE - 1, last.X),
				std::min<s16>(base.Y + MAP_BLOCKSIZE - 1, last.Y),
				std::min<s16>(base.Z + MAP_BLOCKSIZE - 1, last.Z));
		for (s16 z = lo.Z; z <= hi.Z; z++)
		for (s16 y = lo.Y; y <= hi.Y; y++)
		for (s16 x = lo.X; x <= hi.X; x++) {
			const MapNode n = block->getNodeNoCheck(x - base.X, y - base.Y, z - base.Z);
			job.content[vidx(x - origin.X, y - origin.Y, z - origin.Z)] = n.getContent();
		}
	}
}

void LightVolume::compute(const Job &job, std::vector<u8> &out)
{
	// Per-voxel RGB levels 0..15. Same rule as the engine's light: one level
	// lost per step, opaque nodes stop it. Glass multiplies what enters it.
	std::vector<u8> lvl(VOXELS * 3, 0);
	std::vector<u32> queue;
	queue.reserve(8192);
	const size_t ninfo = m_info.size();
	auto info = [&] (u16 c) -> const ContentInfo & {
		static const ContentInfo opaque_unknown;
		return c < ninfo ? m_info[c] : opaque_unknown;
	};

	for (size_t i = 0; i < VOXELS; i++) {
		const ContentInfo &ci = info(job.content[i]);
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
		const s16 x = i % SX, y = (i / SX) % SY, z = i / (SX * SY);
		const u8 *l = &lvl[i * 3];
		if (l[0] <= 1 && l[1] <= 1 && l[2] <= 1)
			continue;
		for (const auto &d : dirs) {
			const s16 nx = x + d[0], ny = y + d[1], nz = z + d[2];
			if (nx < 0 || ny < 0 || nz < 0 || nx >= SX || ny >= SY || nz >= SZ)
				continue;
			const size_t j = vidx(nx, ny, nz);
			const ContentInfo &cj = info(job.content[j]);
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
	m_uploaded_origin = m_result_origin;
	m_have_result = true;
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
			upload(RenderingEngine::get_video_driver());
		}
	}

	// 2. decide whether to start a new job (never two at once)
	m_timer += dtime;
	if (m_busy)
		return;
	const v3s16 half(SX / 2, SY / 2, SZ / 2);
	const v3s16 center = m_job_origin + half;
	const bool moved = std::abs(camera_node.X - center.X) > 8 ||
			std::abs(camera_node.Y - center.Y) > 6 || std::abs(camera_node.Z - center.Z) > 8;
	if (!moved && m_timer < 0.25f)
		return;
	m_timer = 0.f;

	// snap the origin to 4 nodes so small moves don't shift the whole field
	v3s16 origin = camera_node - half;
	origin = v3s16(origin.X & ~3, origin.Y & ~3, origin.Z & ~3);
	const u64 sig = signature(client, origin);
	if (!moved && origin == m_job_origin && sig == m_job_signature)
		return; // nothing changed
	auto job = std::make_unique<Job>();
	snapshot(client, origin, *job);
	m_job_origin = origin;
	m_job_signature = sig;
	m_busy = true;
	{
		std::lock_guard lock(m_mutex);
		m_pending = std::move(job);
	}
	m_cv.notify_one();
}

static std::unique_ptr<LightVolume> s_volume;

LightVolume *lightVolume()
{
	if (!s_volume)
		s_volume = std::make_unique<LightVolume>();
	return s_volume.get();
}

void destroyLightVolume()
{
	s_volume.reset();
}

} // namespace fx
