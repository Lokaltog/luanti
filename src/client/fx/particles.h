// Luanti "fx" fork: GPU particles (sparks, leaves, weather, motes)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A client-side particle system simulated on the GPU with plain OpenGL 3.3
// (no compute, no SSBOs, no instancing):
//
//   state     four RGBA32F textures, one texel per particle slot
//             S0 pos.xyz (nodes, relative to the camera offset), age (s)
//             S1 vel.xyz (nodes/s), kind (0 = dead)
//             S2 life (s), seed (0..1), kind parameter, rest age (<0 flying)
//             S3 tint (rgb packed in 24 bits), leaf layer, size, glow
//             S0..S2 ping-pong between two sets; S3 is written at spawn only.
//   update    a full-target fragment pass (fx_particles_update) per frame:
//             forces (gravity, heat, drag towards the shared wind field,
//             curl turbulence, the player's wake), block-exact collision
//             against the world texture, kind changes (rain -> splash),
//             resting and waking leaves.
//   spawn     the CPU picks emitters and draws one GL point per new particle
//             into the state targets (fx_particles_spawn), slots from a ring
//             that skips slots the CPU knows are still alive. No readback.
//   world     an RGBA8 atlas laid out like the light volume's (96x48x96
//             nodes): r solid, g open to the sky, b liquid (255 water, 128
//             lava). Rebuilt from the light volume's node snapshot.
//   draw      a static mesh of 16384 quads drawn once per chunk of slots at
//             the end of ClientMap's transparent pass (fx_particles_draw):
//             the vertex shader fetches the state and builds camera-facing,
//             velocity-stretched (sparks, rain) or tumbling (leaves) quads;
//             premultiplied alpha, depth-tested, no depth writes.
//
// Everything runs on the main thread. Exists only with caps().gpu_particles.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <SColor.h>
#include <S3DVertex.h>
#include <memory>
#include <random>
#include <string>
#include <vector>

class Client;
class Camera;
namespace video { class IVideoDriver; class ITexture; class IRenderTarget; }
#include <CMeshBuffer.h>

namespace fx
{

class Renderer;

/// Particle kinds; keep in sync with fx_common/particles.glsl (FXP_*).
enum class ParticleKind : u8 {
	Dead = 0,
	Spark = 1,    // lava and fire: hot, rising, cooling, bouncing
	Leaf = 2,     // tumbling, landing, resting, kicked up again
	Rain = 3,
	Splash = 4,   // rain or drip hitting a surface
	Snow = 5,
	Drip = 6,     // rain dripping off leaves
	Dust = 7,     // motes near lights and in daylight
	Ash = 8,      // Nether ash, spores and cinders
	Firefly = 9,
};

class ParticleSystem
{
public:
	/// Null when the driver lacks something (logged).
	static std::unique_ptr<ParticleSystem> create(Renderer &r);
	~ParticleSystem();

	/// Once per frame, before the scene is drawn: emitters, spawn and
	/// update passes (render-to-texture, outside the scene's targets).
	void update(Client *client, Camera *camera, float dtime);
	/// End of ClientMap's transparent pass.
	void draw(video::IVideoDriver *driver);

	u32 capacity() const { return m_width * m_height; }

	/// A new particle (CPU side, before the spawn pass).
	struct Spawn {
		v3f pos;         // world, nodes
		v3f vel;         // nodes/s
		ParticleKind kind;
		float life;      // s
		float param = 0; // kind parameter (ash: rise, leaf: spin)
		video::SColor tint = video::SColor(255, 255, 255, 255);
		float layer = 0; // leaf tile layer
		float size = 0.1f; // nodes
		float glow = 0;  // emission strength
	};

private:
	explicit ParticleSystem(Renderer &r);
	bool init();

	// CPU: tables, the world texture, emitter lists
	void buildTables(Client *client);
	void rebuildWorld(Client *client);
	void emit(Client *client, Camera *camera, float dtime);
	void push(const Spawn &s);
	float rnd() { return m_dist(m_rng); }
	float rnd(float a, float b) { return a + (b - a) * m_dist(m_rng); }

	// GPU
	void uploadBlock(Client *client, Camera *camera, float dtime);
	void runPasses();
	void fullTargetQuad();

	Renderer &m_r;
	video::IVideoDriver *m_driver;

	u32 m_width = 512, m_height = 0;
	video::ITexture *m_state[2][3] = {};  // S0..S2, ping-pong
	video::ITexture *m_static = nullptr;  // S3
	video::IRenderTarget *m_rt_update[2] = {}; // {S0,S1,S2}[i]
	video::IRenderTarget *m_rt_spawn[2] = {};  // {S0,S1,S2}[i] + S3
	int m_cur = 0;                        // set holding the current state
	bool m_cleared = false;
	bool m_prewarmed = false;             // test mode (fx_test_particle_prewarm)

	video::ITexture *m_world = nullptr;   // collision / sky atlas
	video::ITexture *m_leaf_tiles = nullptr; // array texture of leaf tiles
	scene::SMeshBuffer *m_quads = nullptr;
	u32 m_shader_update = 0, m_shader_spawn = 0, m_shader_draw = 0;

	// slot ring: CPU-known death time per slot (an upper bound)
	std::vector<float> m_death;
	u32 m_cursor = 0;
	u32 m_high = 0;           // slots ever used (draw this many)
	float m_time = 0.f;
	std::vector<video::S3DVertexTangents> m_spawns;
	u32 m_dropped = 0;
	u32 m_stat_spawned = 0;   // for the 2 s info line
	float m_stat_timer = 0.f;

	// per content: what it is to particles
	struct ContentInfo {
		u8 flags = 0;
		s8 leaf_layer = -1;
		bool flame = false; // an open flame light (torch, candle, campfire): sparks
	};
	std::vector<ContentInfo> m_info;
	bool m_tables_built = false;

	// world snapshot derived data (rebuilt with the world texture)
	u32 m_world_generation = ~0u;
	float m_world_timer = 0.f;
	v3s16 m_world_origin;      // snapshot origin of the uploaded world texture
	bool m_world_valid = false;
	std::vector<u8> m_world_px; // CPU copy (ARGB8 atlas)
	std::vector<s16> m_column_top; // per (x,z): highest rain-blocking y index, -1 none
	std::vector<u8> m_column_flags; // flags of that top voxel

	struct Emitters {
		std::vector<v3s16> lava, fire, leaves, canopy_leaves, flora, lights, flames;
	} m_emit;
	float m_acc[16] = {};     // fractional spawn accumulators

	// frame state
	v3s16 m_anchor;           // = the camera offset (nodes)
	v3s16 m_prev_anchor;
	bool m_have_anchor = false;
	float m_dtime = 0.f;
	v3f m_cam_pos;            // nodes, world

	std::mt19937 m_rng;
	std::uniform_real_distribution<float> m_dist{0.f, 1.f};
};

} // namespace fx
