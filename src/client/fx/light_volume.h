// Luanti "fx" fork: M4 RGB light volume
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A leak-free coloured light field around the camera. The engine's light is a
// single 4-bit scalar per node; this computes the same flood fill per RGB
// channel, client-side, so colour only goes where light can travel, and
// stained glass multiplies what passes through it.
//
// Main thread: snapshot node content IDs around the camera, upload results.
// Worker thread: per-channel BFS. The result is a 2D atlas texture (Y slices
// tiled in a grid) sampled with manual trilinear filtering in the shaders.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <SColor.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Client;
class NodeDefManager;
class ITextureSource;
namespace video { class ITexture; class IVideoDriver; }

namespace fx
{

class LightVolume
{
public:
	// Volume size in nodes. Light reaches 14 nodes, so this covers what the
	// camera sees up close; beyond it the shaders fall back to T1 chroma.
	static constexpr s16 SX = 64, SY = 48, SZ = 64;
	static constexpr s16 ATLAS_COLS = 8;
	static constexpr u32 ATLAS_W = SX * ATLAS_COLS;
	static constexpr u32 ATLAS_H = SZ * ((SY + ATLAS_COLS - 1) / ATLAS_COLS);

	LightVolume();
	~LightVolume();

	/// Main thread, once per frame: decide whether to recompute, snapshot,
	/// hand off to the worker, and upload a finished result.
	void update(Client *client, v3s16 camera_node, float dtime);

	/// The texture to bind (null until the first result is uploaded).
	video::ITexture *texture() const { return m_texture; }
	/// Node coordinate of voxel (0,0,0) of the uploaded result.
	v3s16 origin() const { return m_uploaded_origin; }
	bool valid() const { return m_texture != nullptr && m_have_result; }

	/// Forget per-content tables and results (media reload, settings change).
	void reset();

private:
	struct ContentInfo {
		bool opaque = true;
		u8 src[3] = {0, 0, 0};       // source level per channel, 0..15
		u8 tint[3] = {255, 255, 255}; // transmission per channel (glass), /255
	};

	struct Job {
		v3s16 origin;
		std::vector<u16> content;   // SX*SY*SZ content ids (CONTENT_IGNORE = unknown)
	};

	void buildTables(Client *client);
	u64 signature(Client *client, v3s16 origin);
	void snapshot(Client *client, v3s16 origin, Job &job);
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
	bool m_result_ready = false;
	std::atomic<bool> m_busy{false};

	// main-thread state
	video::ITexture *m_texture = nullptr;
	v3s16 m_uploaded_origin;
	bool m_have_result = false;
	v3s16 m_job_origin{-32768, -32768, -32768};
	u64 m_job_signature = 0;
	float m_timer = 0.f;
};

/// The process-wide volume (created on first use when fx_light_volume is on).
LightVolume *lightVolume();
void destroyLightVolume();

} // namespace fx
