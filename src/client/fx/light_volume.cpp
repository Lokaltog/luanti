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
#include <unordered_map>

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
	const NodeDefManager *ndef = client->ndef();
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
			for (s16 x = l.X; x <= h.X; x++) {
				const size_t i = row + (x - m_origin.X);
				if (!block) {
					m_content[i] = CONTENT_IGNORE;
					m_engine[i] = 0;
					continue;
				}
				const MapNode n = block->getNodeNoCheck(x - base.X, y - base.Y, z - base.Z);
				m_content[i] = n.getContent();
				// low nibble: night bank (artificial), high: day bank (sun)
				const ContentLightingFlags lf = ndef->getLightingFlags(n);
				m_engine[i] = n.getLight(LIGHTBANK_NIGHT, lf) | (n.getLight(LIGHTBANK_DAY, lf) << 4);
			}
		}
	}
	return true;
}

void LightVolume::moveTo(Client *client, v3s16 origin)
{
	const v3s16 size(SX, SY, SZ);
	const v3s16 d = origin - m_origin;
	std::vector<u16> old;
	std::vector<u8> old_engine;
	old.swap(m_content);
	old_engine.swap(m_engine);
	m_content.assign(VOXELS, CONTENT_IGNORE);
	m_engine.assign(VOXELS, 0);
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
	{
		std::memcpy(&m_content[vidx(lo.X, y, z)], &old[vidx(lo.X + d.X, y + d.Y, z + d.Z)],
				(hi.X - lo.X + 1) * sizeof(u16));
		std::memcpy(&m_engine[vidx(lo.X, y, z)], &old_engine[vidx(lo.X + d.X, y + d.Y, z + d.Z)],
				hi.X - lo.X + 1);
	}
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
	static const u8 white[3] = {255, 255, 255};
	floodFillRGB(sx, sy, sz, content, nullptr, cells, white, lvl);
}

void floodFillRGB(s16 sx, s16 sy, s16 sz, const u16 *content, const u8 *engine,
		const std::vector<LightCell> &cells, const u8 fallback[3], std::vector<u8> &lvl)
{
	const size_t voxels = (size_t)sx * sy * sz;
	lvl.assign(voxels * 3, 0);
	// the engine's scalar rule alongside (tints don't dim it), to tell
	// light the fill can't explain from light glass took away
	std::vector<u8> scalar(engine ? voxels : 0, 0);
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
			if (engine)
				scalar[i] = std::max({ci.src[0], ci.src[1], ci.src[2]});
			queue.push_back((u32)i);
		}
	}

	static const s16 dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	auto spread = [&] () {
		for (size_t head = 0; head < queue.size(); head++) {
			const u32 i = queue[head];
			const s16 x = i % sx, y = (i / sx) % sy, z = i / ((u32)sx * sy);
			const u8 l[3] = {lvl[i * 3], lvl[i * 3 + 1], lvl[i * 3 + 2]};
			const u8 ls = engine ? scalar[i] : 0;
			if (l[0] <= 1 && l[1] <= 1 && l[2] <= 1 && ls <= 1)
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
				if (engine && ls > 1 && ls - 1 > scalar[j]) {
					scalar[j] = ls - 1;
					improved = true;
				}
				if (improved)
					queue.push_back((u32)j);
			}
		}
	};
	spread();
	if (!engine)
		return;

	// seed what the engine lights but the fill doesn't explain
	queue.clear();
	for (size_t i = 0; i < voxels; i++) {
		const u8 e = std::min<u8>(engine[i], 15);
		if (e <= scalar[i] || info(content[i]).opaque)
			continue;
		scalar[i] = e;
		for (int c = 0; c < 3; c++)
			lvl[i * 3 + c] = std::max<u8>(lvl[i * 3 + c], (u8)((e * fallback[c] + 127) / 255));
		queue.push_back((u32)i);
	}
	spread();
}

void LightVolume::compute(const Job &job, std::vector<u8> &out)
{
	// hue for light whose source isn't in the snapshot: the average hue of
	// the sources that are (torches, mostly), else white
	float acc[3] = {0.f, 0.f, 0.f};
	for (u16 c : job.content) {
		if (c >= m_info.size())
			continue;
		const u8 *src = m_info[c].src;
		const float m = std::max({src[0], src[1], src[2]});
		if (m > 0.f)
			for (int i = 0; i < 3; i++)
				acc[i] += src[i] / m;
	}
	u8 fallback[3] = {255, 255, 255};
	const float am = std::max({acc[0], acc[1], acc[2]});
	if (am > 0.f)
		for (int i = 0; i < 3; i++)
			fallback[i] = (u8)(acc[i] / am * 255.f + 0.5f);

	std::vector<u8> night(VOXELS), day(VOXELS);
	for (size_t i = 0; i < VOXELS; i++) {
		night[i] = job.engine[i] & 15;
		day[i] = job.engine[i] >> 4;
	}
	std::vector<u8> lvl;
	floodFillRGB(SX, SY, SZ, job.content.data(), night.data(), m_info, fallback, lvl);
	std::vector<u8> sky;
	skyTint(SX, SY, SZ, job.content.data(), day.data(), m_info, sky);

	// Pack into the atlas: Y slices tiled ATLAS_COLS per row, ARGB8 texels.
	// The upper half holds the light levels, the lower half the sky tint.
	out.assign((size_t)ATLAS_W * ATLAS_TEX_H * 4, 0);
	u32 *px = reinterpret_cast<u32 *>(out.data());
	for (s16 y = 0; y < SY; y++) {
		const u32 tx = (y % ATLAS_COLS) * SX, ty = (y / ATLAS_COLS) * SZ;
		for (s16 z = 0; z < SZ; z++)
		for (s16 x = 0; x < SX; x++) {
			const u8 *l = &lvl[vidx(x, y, z) * 3];
			const u32 r = l[0] * 17, g = l[1] * 17, b = l[2] * 17;
			const u32 a = 0; // alpha: the held light (applyHeld)
			px[(ty + z) * ATLAS_W + tx + x] = video::SColor(a, r, g, b).color;
			const u8 *s = &sky[vidx(x, y, z) * 3];
			px[(ATLAS_H + ty + z) * ATLAS_W + tx + x] = video::SColor(255, s[0], s[1], s[2]).color;
		}
	}
}

void skyTint(s16 sx, s16 sy, s16 sz, const u16 *content, const u8 *day,
		const std::vector<LightCell> &cells, std::vector<u8> &out)
{
	// Follow the engine's own daylight back to where it came from: a node at
	// level 15 got it straight from above (sunlight falls through anything
	// that lets it), one at level L < 15 from neighbours at L + 1. Its tint
	// is theirs, times what its own cell transmits. Light from outside the
	// window, or with no traceable source, stays white.
	const size_t n = (size_t)sx * sy * sz;
	out.assign(n * 3, 255);
	auto idx = [&] (s16 x, s16 y, s16 z) { return ((size_t)z * sy + y) * sx + x; };
	auto cell_tint = [&] (size_t i) -> const u8 * {
		static const u8 white[3] = {255, 255, 255};
		const u16 c = content[i];
		return c < cells.size() ? cells[c].tint : white;
	};
	// level 15: top down per column
	for (s16 z = 0; z < sz; z++)
	for (s16 x = 0; x < sx; x++)
	for (s16 y = sy - 1; y >= 0; y--) {
		const size_t i = idx(x, y, z);
		if (day[i] != 15)
			continue;
		const u8 *t = cell_tint(i);
		const u8 *up = (y + 1 < sy && day[idx(x, y + 1, z)] == 15) ? &out[idx(x, y + 1, z) * 3] : nullptr;
		for (int ch = 0; ch < 3; ch++)
			out[i * 3 + ch] = (u8)(((up ? up[ch] : 255) * t[ch] + 127) / 255);
	}
	// lower levels, brightest first, so every source is done before its
	// receivers
	std::vector<std::vector<u32>> by_level(15);
	for (size_t i = 0; i < n; i++)
		if (day[i] > 0 && day[i] < 15)
			by_level[day[i]].push_back((u32)i);
	static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int L = 14; L >= 1; L--) {
		for (u32 i : by_level[L]) {
			const s16 x = i % sx, y = (i / sx) % sy, z = i / ((size_t)sx * sy);
			u32 acc[3] = {0, 0, 0}, cnt = 0;
			for (const auto &d : D) {
				const s16 nx = x + d[0], ny = y + d[1], nz = z + d[2];
				if (nx < 0 || ny < 0 || nz < 0 || nx >= sx || ny >= sy || nz >= sz)
					continue;
				const size_t j = idx(nx, ny, nz);
				if (day[j] < L + 1)
					continue;
				for (int ch = 0; ch < 3; ch++)
					acc[ch] += out[j * 3 + ch];
				cnt++;
			}
			if (!cnt)
				continue; // from outside the window: white
			const u8 *t = cell_tint(i);
			for (int ch = 0; ch < 3; ch++)
				out[i * 3 + ch] = (u8)((acc[ch] / cnt * t[ch] + 127) / 255);
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
		m_texture = driver->addTexture(core::dimension2du(ATLAS_W, ATLAS_TEX_H),
				"fx_light_volume", video::ECF_A8R8G8B8);
		driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, mip);
		if (!m_texture) {
			warningstream << "fx: could not create the light volume texture" << std::endl;
			return;
		}
	}
	m_uploaded.swap(m_result);
	m_uploaded_origin = m_result_origin;
	m_uploaded_generation = m_result_generation;
	m_have_result = true;
	m_held_texels.clear(); // a fresh atlas has no held light yet
	applyHeld();           // writes the texture
}

void LightVolume::writeTexture()
{
	if (!m_texture || m_uploaded.empty())
		return;
	void *dst = m_texture->lock(video::ETLM_WRITE_ONLY);
	if (!dst)
		return;
	const u32 pitch = m_texture->getPitch();
	for (u32 row = 0; row < ATLAS_TEX_H; row++)
		std::memcpy((u8 *)dst + row * pitch, m_uploaded.data() + (size_t)row * ATLAS_W * 4, ATLAS_W * 4);
	m_texture->unlock();
}

void LightVolume::setHeld(v3s16 node, u8 level)
{
	level = std::min<u8>(level, 14);
	if (level == m_held_level && (level == 0 || node == m_held_node))
		return;
	// a full atlas upload per change: at most every 120 ms while moving (the
	// shader glides the light between updates; picking the item up or
	// putting it away goes through at once)
	const u64 now = porting::getTimeMs();
	if (level == m_held_level && now - m_held_upload_ms < 120)
		return;
	m_held_upload_ms = now;
	m_held_node = node;
	m_held_level = level;
	if (m_have_result)
		applyHeld();
}

void LightVolume::applyHeld()
{
	u32 *px = reinterpret_cast<u32 *>(m_uploaded.data());
	for (u32 t : m_held_texels)
		px[t] &= 0x00ffffffu;
	m_held_texels.clear();
	auto texel = [&] (v3s16 a) -> s64 { // atlas texel of an absolute node, or -1
		const v3s16 v = a - m_uploaded_origin;
		if (v.X < 0 || v.Y < 0 || v.Z < 0 || v.X >= SX || v.Y >= SY || v.Z >= SZ)
			return -1;
		return (s64)((v.Y / ATLAS_COLS) * SZ + v.Z) * ATLAS_W + (v.Y % ATLAS_COLS) * SX + v.X;
	};
	auto opaque = [&] (v3s16 a) { // from the current snapshot; unknown counts as opaque
		const v3s16 v = a - m_origin;
		if (!m_have_content || v.X < 0 || v.Y < 0 || v.Z < 0 || v.X >= SX || v.Y >= SY || v.Z >= SZ)
			return true;
		const u16 c = m_content[vidx(v.X, v.Y, v.Z)];
		return c >= m_info.size() || m_info[c].opaque;
	};
	if (m_held_level > 0) {
		// the engine's rule: one level per step, through what light passes;
		// a flat box around the source (light reaches 14 nodes)
		constexpr s16 R = 14, D = 2 * R + 1;
		static thread_local std::vector<u8> seen;
		seen.assign((size_t)D * D * D, 0);
		auto bi = [&] (v3s16 q) -> s32 {
			const v3s16 o = q - m_held_node + v3s16(R, R, R);
			if (o.X < 0 || o.Y < 0 || o.Z < 0 || o.X >= D || o.Y >= D || o.Z >= D)
				return -1;
			return ((s32)o.Z * D + o.Y) * D + o.X;
		};
		std::vector<std::pair<v3s16, u8>> queue{{m_held_node, m_held_level}};
		queue.reserve(8192);
		seen[bi(m_held_node)] = m_held_level;
		static const v3s16 dirs[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
		for (size_t head = 0; head < queue.size(); head++) {
			const auto [p, l] = queue[head];
			const s64 t = texel(p);
			if (t >= 0) {
				px[t] = (px[t] & 0x00ffffffu) | ((u32)l * 17u) << 24;
				m_held_texels.push_back((u32)t);
			}
			if (l <= 1)
				continue;
			for (const v3s16 &d : dirs) {
				const v3s16 q = p + d;
				const s32 b = bi(q);
				if (b < 0 || seen[b] >= l - 1 || opaque(q))
					continue;
				seen[b] = l - 1;
				queue.emplace_back(q, (u8)(l - 1));
			}
		}
	}
	writeTexture();
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
		m_engine.assign(VOXELS, 0);
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
	job->engine = m_engine;
	m_job_generation = m_generation;
	m_busy = true;
	{
		std::lock_guard lock(m_mutex);
		m_pending = std::move(job);
	}
	m_cv.notify_one();
}

} // namespace fx
