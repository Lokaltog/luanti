// Luanti "fx" fork: M4 RGB light volume
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A leak-free coloured light field around the camera. The engine's light is a
// single 4-bit scalar per node; this computes the same flood fill per RGB
// channel, client-side, so colour only goes where light can travel, and
// stained glass multiplies what passes through it.
//
// Main thread: keeps a snapshot of node content IDs around the camera,
// updated incrementally (blocks the client re-meshes are marked dirty and
// re-read; moving the window only reads the newly exposed slabs). Each change
// bumps a generation counter; a new fill is handed to the worker only when
// the generation moved. Worker thread: per-channel BFS over the snapshot.
// The result is a 2D atlas texture (Y slices tiled in a grid), sampled with
// hardware bilinear in XZ and a manual lerp in Y.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <SColor.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

class Client;
class NodeDefManager;
class ITextureSource;
namespace video { class ITexture; class IVideoDriver; }

namespace fx
{

/// How one content type affects light in the flood fill.
struct LightCell {
	bool opaque = true;
	u8 src[3] = {0, 0, 0};        // source level per channel, 0..15
	u8 tint[3] = {255, 255, 255}; // transmission per channel (glass), /255
};

/// The engine's light rule per RGB channel: one level lost per step, opaque
/// cells stop it, tinted cells multiply what enters them. `content` holds
/// sx*sy*sz indices into `cells` (x fastest, then y, then z); unknown indices
/// count as opaque. Writes sx*sy*sz*3 levels (0..15) to `out`.
void floodFillRGB(s16 sx, s16 sy, s16 sz, const u16 *content,
		const std::vector<LightCell> &cells, std::vector<u8> &out);

/// Same, reconciled with the engine's own artificial light (`engine`: the
/// night light bank per cell, 0..15). Light the fill can't explain (its
/// source sits in a block the client hasn't received, e.g. around a corner
/// the server culls, or outside the window) is seeded where the engine says
/// it is, in `fallback` hue (0..255 per channel, brightest 255), and spread
/// by the same rule. Without this such light would be white until the
/// source's block arrives.
void floodFillRGB(s16 sx, s16 sy, s16 sz, const u16 *content, const u8 *engine,
		const std::vector<LightCell> &cells, const u8 fallback[3], std::vector<u8> &out);

class LightVolume
{
public:
	// Volume size in nodes. Light reaches 14 nodes, so this covers what the
	// camera sees up close; beyond it light is white (upstream).
	static constexpr s16 SX = 96, SY = 48, SZ = 96;
	static constexpr s16 ATLAS_COLS = 8;
	static constexpr u32 ATLAS_W = SX * ATLAS_COLS;
	static constexpr u32 ATLAS_H = SZ * ((SY + ATLAS_COLS - 1) / ATLAS_COLS);

	explicit LightVolume(video::IVideoDriver *driver);
	~LightVolume();

	/// Main thread, once per frame: follow the camera, re-read dirty blocks,
	/// hand a changed snapshot to the worker, upload a finished result.
	void update(Client *client, v3s16 camera_node, float dtime);

	/// Main thread: the client changed this block (new data or a node edit).
	void noteBlockChanged(v3s16 blockpos) { m_dirty.insert(blockpos); }

	/// The texture to bind (null until the first result is uploaded).
	video::ITexture *texture() const { return m_texture; }
	/// Node coordinate of voxel (0,0,0) of the uploaded result.
	v3s16 origin() const { return m_uploaded_origin; }
	bool valid() const { return m_texture != nullptr && m_have_result; }
	/// Snapshot generation of the uploaded result (bumps on every change).
	u32 generation() const { return m_uploaded_generation; }
	/// Light level per channel (0..15) at a node, from the uploaded result.
	/// False outside the window or before the first result. Main thread.
	bool sampleLevels(v3s16 node, u8 out[3]) const;

	/// The uploaded result matches the current snapshot (tests wait for it).
	bool current() const
	{
		return m_have_result && m_uploaded_generation == m_generation && m_dirty.empty();
	}

private:
	using ContentInfo = LightCell;

	struct Job {
		v3s16 origin;
		u32 generation = 0;
		std::vector<u16> content;   // SX*SY*SZ content ids (CONTENT_IGNORE = unknown)
		std::vector<u8> engine;     // SX*SY*SZ engine night light, 0..15
	};

	void buildTables(Client *client);
	/// Re-read the nodes of [lo, hi] (inclusive, node coordinates, clipped
	/// to the window) into m_content. True if anything was in the window.
	bool readBox(Client *client, v3s16 lo, v3s16 hi);
	/// Move the window to `origin`, keeping the overlap.
	void moveTo(Client *client, v3s16 origin);
	void compute(const Job &job, std::vector<u8> &out_rgba);
	void workerLoop();
	void upload(video::IVideoDriver *driver);

	std::vector<ContentInfo> m_info;
	bool m_tables_built = false;

	// worker handoff
	std::thread m_thread;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_quit = false;
	std::unique_ptr<Job> m_pending;      // main -> worker
	std::vector<u8> m_result;            // worker -> main (RGBA atlas)
	v3s16 m_result_origin;
	u32 m_result_generation = 0;
	bool m_result_ready = false;
	std::atomic<bool> m_busy{false};

	// main-thread state: the snapshot window
	video::IVideoDriver *m_driver;
	std::vector<u16> m_content;
	std::vector<u8> m_engine;            // engine night light per voxel
	v3s16 m_origin;
	bool m_have_content = false;
	std::unordered_set<v3s16> m_dirty;
	u32 m_generation = 0, m_job_generation = 0;
	float m_timer = 0.f;

	video::ITexture *m_texture = nullptr;
	std::vector<u8> m_uploaded;          // the atlas last uploaded (CPU copy)
	v3s16 m_uploaded_origin;
	u32 m_uploaded_generation = 0;
	bool m_have_result = false;
};

} // namespace fx
