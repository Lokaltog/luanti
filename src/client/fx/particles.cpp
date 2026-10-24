// Luanti "fx" fork: GPU particles (sparks, leaves, weather, motes)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// See particles.h for the architecture. This file: GPU resources and passes,
// the world texture, the per-frame uniform block. Emitters: particle_emit.cpp.

#include "particles.h"
#include "renderer.h"
#include "fx_test.h"

#include "client/camera.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/localplayer.h"
#include "client/node_visuals.h"
#include "client/shader.h"
#include "client/texturesource.h"
#include "itemgroup.h"
#include "exceptions.h"
#include "log.h"
#include "nodedef.h"
#include "settings.h"
#include <CMeshBuffer.h>
#include <ICameraSceneNode.h>
#include <IRenderTarget.h>
#include <IVideoDriver.h>
#include <ITexture.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace fx
{

/// Uniform block binding of FxParticles (fx_common/particles.glsl).
constexpr u32 UBO_PARTICLES = 4;
/// Quads per draw call: 16-bit indices, 4 vertices per quad.
constexpr u32 CHUNK = 0x4000;

/// std140 mirror of FxParticles in fx_common/particles.glsl.
struct ParticleBlock {
	float frame[4];      // dt, time, state width, state height
	float shift[4];      // anchor change this frame (nodes, old - new), wake strength
	float world[4];      // world texture origin relative to the anchor (nodes), valid
	float anchor[4];     // anchor = camera offset (world nodes), -
	float player[4];     // player centre relative to the anchor (nodes), speed (nodes/s)
	float player_vel[4]; // nodes/s, -
	float cam_right[4];  // camera right, daylight (0..1)
	float cam_up[4];     // camera up, night (0..1)
	float cam_pos[4];    // camera relative to the anchor (nodes), -
	float sky[4];        // sky light colour (linear), ambient floor
};
static_assert(sizeof(ParticleBlock) == 10 * 16, "FxParticles std140 layout");

std::unique_ptr<ParticleSystem> ParticleSystem::create(Renderer &r)
{
	std::unique_ptr<ParticleSystem> ps(new ParticleSystem(r));
	if (!ps->init())
		return nullptr;
	return ps;
}

ParticleSystem::ParticleSystem(Renderer &r) : m_r(r), m_driver(r.driver)
{
	// test mode: the same particles every run
	m_rng.seed(test::active() ? 1234u : std::random_device()());
}

ParticleSystem::~ParticleSystem()
{
	for (auto *rt : m_rt_update)
		if (rt)
			m_driver->removeRenderTarget(rt);
	for (auto *rt : m_rt_spawn)
		if (rt)
			m_driver->removeRenderTarget(rt);
	for (auto &set : m_state)
		for (auto *t : set)
			if (t)
				m_driver->removeTexture(t);
	if (m_static)
		m_driver->removeTexture(m_static);
	if (m_world)
		m_driver->removeTexture(m_world);
	if (m_quads)
		m_quads->drop(); // its hardware buffers go with it (like Sky's stars)
	// m_leaf_tiles belongs to the TextureSource
}

bool ParticleSystem::init()
{
	const u32 budget = (u32)std::clamp(m_r.base.particle_budget, 4096, 1048576);
	m_height = (budget + m_width - 1) / m_width;
	const core::dimension2du size(m_width, m_height);

	m_driver->setUniformBlockBinding("FxParticles", UBO_PARTICLES);
	{
		ParticleBlock b{};
		m_driver->setUniformBufferData(UBO_PARTICLES, &b, sizeof(b));
	}

	const char *names[3] = {"fx_particles_s0", "fx_particles_s1", "fx_particles_s2"};
	for (int i = 0; i < 2; i++)
		for (int k = 0; k < 3; k++) {
			m_state[i][k] = m_driver->addRenderTargetTexture(size,
					std::string(names[k]) + (i ? "b" : "a"), video::ECF_A32B32G32R32F);
			if (!m_state[i][k])
				return false;
		}
	m_static = m_driver->addRenderTargetTexture(size, "fx_particles_s3", video::ECF_A32B32G32R32F);
	if (!m_static)
		return false;
	for (int i = 0; i < 2; i++) {
		core::array<video::ITexture *> upd, spn;
		for (int k = 0; k < 3; k++) {
			upd.push_back(m_state[i][k]);
			spn.push_back(m_state[i][k]);
		}
		spn.push_back(m_static);
		m_rt_update[i] = m_driver->addRenderTarget();
		m_rt_spawn[i] = m_driver->addRenderTarget();
		if (!m_rt_update[i] || !m_rt_spawn[i])
			return false;
		m_rt_update[i]->setTexture(upd, nullptr);
		m_rt_spawn[i]->setTexture(spn, nullptr);
	}

	// the draw mesh: CHUNK quads, TCoords = (quad index in the chunk, corner)
	m_quads = new scene::SMeshBuffer();
	auto &vert = m_quads->Vertices->Data;
	auto &idx = m_quads->Indices->Data;
	vert.reserve(CHUNK * 4);
	idx.reserve(CHUNK * 6);
	for (u32 q = 0; q < CHUNK; q++) {
		for (int c = 0; c < 4; c++)
			vert.emplace_back(v3f(0.f), v3f(0.f, 1.f, 0.f), video::SColor(0xffffffff),
					v2f((float)q, (float)c));
		const u16 b = (u16)(q * 4);
		for (u16 o : {0, 1, 2, 2, 3, 0})
			idx.push_back(b + o);
	}
	m_quads->setHardwareMappingHint(scene::EHM_STATIC);
	m_quads->recalculateBoundingBox();

	m_death.assign(capacity(), 0.f);

	IWritableShaderSource *shsrc = m_r.shsrc;
	try {
		m_shader_update = shsrc->getShader("fx_particles_update", {}, video::EMT_SOLID);
		m_shader_spawn = shsrc->getShader("fx_particles_spawn", {}, video::EMT_SOLID);
		m_shader_draw = shsrc->getShader("fx_particles_draw", {}, video::EMT_SOLID);
	} catch (const ShaderException &e) {
		// a broken particle shader turns particles off, not the game
		errorstream << "fx: GPU particle shaders failed to build: " << e.what() << std::endl;
		return false;
	}
	if (!m_shader_update || !m_shader_spawn || !m_shader_draw) {
		errorstream << "fx: GPU particle shaders failed to build" << std::endl;
		return false;
	}
	infostream << "fx: GPU particles, " << capacity() << " slots (" << m_width << "x"
		<< m_height << " RGBA32F x7)" << std::endl;
	return true;
}

/*
	Content tables and the world texture
*/

enum : u8 {
	F_SOLID = 1, F_WATER = 2, F_LAVA = 4, F_FIRE = 8,
	F_LEAVES = 16, F_FLORA = 32, F_LIGHT = 64, F_RAINBLOCK = 128,
};

static bool nameHas(const std::string &name, const char *word)
{
	return name.find(word) != std::string::npos;
}

void ParticleSystem::buildTables(Client *client)
{
	const NodeDefManager *ndef = client->ndef();
	m_info.assign(ndef->size(), ContentInfo());
	std::vector<std::string> tiles;
	std::map<std::string, int> tile_layer;
	for (content_t c = 0; c < ndef->size(); c++) {
		if (c == CONTENT_AIR || c == CONTENT_IGNORE)
			continue;
		const ContentFeatures &f = ndef->get(c);
		if (f.name.empty())
			continue;
		auto g = [&] (const char *group) { return itemgroup_get(f.groups, group) > 0; };
		const bool liquid = f.liquid_type != LIQUID_NONE;
		const bool lava = g("lava") || (liquid && nameHas(f.name, "lava"));
		const bool water = !lava && liquid && (g("water") || nameHas(f.name, "water") ||
				detectSurface(f).kind == SurfaceKind::Water);
		const bool fire = g("fire") || f.drawtype == NDT_FIRELIKE;
		const bool allfaces = f.drawtype == NDT_ALLFACES || f.drawtype == NDT_ALLFACES_OPTIONAL;
		const bool leaves = g("leaves") || f.waving == 2 || (allfaces && nameHas(f.name, "leaves"));
		const bool thin = f.drawtype == NDT_AIRLIKE || f.drawtype == NDT_PLANTLIKE ||
				f.drawtype == NDT_FIRELIKE || f.drawtype == NDT_TORCHLIKE ||
				f.drawtype == NDT_SIGNLIKE || f.drawtype == NDT_RAILLIKE;
		const bool flora = !leaves && (g("flora") || g("flower") || g("grass") || g("plant") ||
				(f.drawtype == NDT_PLANTLIKE && !f.walkable && f.light_source == 0));
		const bool solid = f.walkable && !thin;
		u8 fl = 0;
		if (solid) fl |= F_SOLID;
		if (water) fl |= F_WATER;
		if (lava) fl |= F_LAVA;
		if (fire) fl |= F_FIRE;
		if (leaves) fl |= F_LEAVES;
		if (flora) fl |= F_FLORA;
		if (f.light_source >= 8 && !liquid && !fire) fl |= F_LIGHT;
		if (solid || liquid) fl |= F_RAINBLOCK;
		m_info[c].flags = fl;

		// leaf tiles: one array layer per distinct tile (up to 32)
		if (leaves && !f.tiledef[0].name.empty()) {
			const std::string &t = f.tiledef[0].name;
			auto it = tile_layer.find(t);
			if (it != tile_layer.end()) {
				m_info[c].leaf_layer = (s8)it->second;
			} else if (tiles.size() < 32) {
				tile_layer[t] = (int)tiles.size();
				m_info[c].leaf_layer = (s8)tiles.size();
				tiles.push_back(t);
			}
		}
	}
	if (tiles.empty())
		tiles.push_back("[fill:16x16:#50a040"); // a green square stands in
	for (auto &t : tiles)
		t = "(" + t + ")^[resize:16x16";
	m_leaf_tiles = client->getTextureSource()->addArrayTexture(tiles);
	m_tables_built = true;
	infostream << "fx: GPU particle tables for " << ndef->size() << " contents, "
		<< tiles.size() << " leaf tiles" << std::endl;
}

// Reservoir sampling: a uniform pick of at most `cap` of everything offered.
template <typename R>
static void offer(std::vector<v3s16> &list, u32 &seen, v3s16 p, size_t cap, R &rng)
{
	seen++;
	if (list.size() < cap) {
		list.push_back(p);
		return;
	}
	const u32 j = std::uniform_int_distribution<u32>(0, seen - 1)(rng);
	if (j < cap)
		list[j] = p;
}

void ParticleSystem::rebuildWorld(Client *client)
{
	const LightVolume &vol = *m_r.volume;
	const std::vector<u16> &snap = vol.snapshot();
	constexpr s16 SX = LightVolume::SX, SY = LightVolume::SY, SZ = LightVolume::SZ;
	constexpr u32 AW = LightVolume::ATLAS_W, AH = LightVolume::ATLAS_H;
	constexpr s16 COLS = LightVolume::ATLAS_COLS;
	if (snap.size() != (size_t)SX * SY * SZ)
		return;
	const v3s16 origin = vol.snapshotOrigin();

	m_world_px.assign((size_t)AW * AH * 4, 0);
	u32 *px = reinterpret_cast<u32 *>(m_world_px.data());
	m_column_top.assign((size_t)SX * SZ, -1);
	m_column_flags.assign((size_t)SX * SZ, 0);
	Emitters e;
	u32 seen[6] = {};
	constexpr size_t CAP = 2048;

	auto flags = [&] (s16 x, s16 y, s16 z) -> u8 {
		const u16 c = snap[((size_t)z * SY + y) * SX + x];
		return c < m_info.size() ? m_info[c].flags : 0;
	};
	const u8 BLOCK = F_SOLID | F_WATER | F_LAVA;
	for (s16 z = 0; z < SZ; z++)
	for (s16 x = 0; x < SX; x++) {
		bool open = true;
		u8 top_flags = 0;
		const size_t col = (size_t)z * SX + x;
		u8 above = 0;
		for (s16 y = SY - 1; y >= 0; y--) {
			const u8 fl = flags(x, y, z);
			if (open && (fl & F_RAINBLOCK)) {
				open = false;
				m_column_top[col] = y;
				m_column_flags[col] = top_flags = fl;
			}
			const u32 r = (fl & F_SOLID) ? 255 : 0;
			const u32 g = open ? 255 : 0;
			const u32 b = (fl & F_WATER) ? 255 : (fl & F_LAVA) ? 128 : 0;
			const u32 tx = (y % COLS) * SX + x, ty = (y / COLS) * SZ + z;
			px[(size_t)ty * AW + tx] = video::SColor(255, r, g, b).color;

			if (fl & (F_LAVA | F_FIRE | F_LEAVES | F_FLORA | F_LIGHT)) {
				const u8 below = y > 0 ? flags(x, y - 1, z) : (u8)F_SOLID;
				const bool air_above = !(above & BLOCK);
				const bool air_below = !(below & BLOCK) && !(below & F_LEAVES);
				const v3s16 p = origin + v3s16(x, y, z);
				if ((fl & F_LAVA) && air_above)
					offer(e.lava, seen[0], p, CAP, m_rng);
				if (fl & F_FIRE)
					offer(e.fire, seen[1], p, CAP, m_rng);
				if ((fl & F_LEAVES) && air_below) {
					offer(e.leaves, seen[2], p, CAP, m_rng);
					// rain falls on this tree's canopy: drips from below
					if (top_flags & F_LEAVES)
						offer(e.canopy_leaves, seen[3], p, CAP, m_rng);
				}
				if ((fl & F_FLORA) && open && !(above & BLOCK))
					offer(e.flora, seen[4], p, CAP, m_rng);
				if (fl & F_LIGHT)
					offer(e.lights, seen[5], p, CAP, m_rng);
			}
			above = fl;
		}
	}
	m_emit = std::move(e);

	if (!m_world) {
		const bool mip = m_driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS);
		m_driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, false);
		m_world = m_driver->addTexture(core::dimension2du(AW, AH), "fx_particles_world",
				video::ECF_A8R8G8B8);
		m_driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, mip);
		if (!m_world) {
			warningstream << "fx: could not create the particle world texture" << std::endl;
			return;
		}
	}
	void *dst = m_world->lock(video::ETLM_WRITE_ONLY);
	if (!dst)
		return;
	const u32 pitch = m_world->getPitch();
	for (u32 row = 0; row < AH; row++)
		std::memcpy((u8 *)dst + row * pitch, m_world_px.data() + (size_t)row * AW * 4, AW * 4);
	m_world->unlock();
	m_world_origin = origin;
	m_world_valid = true;
	verbosestream << "fx: particle world rebuilt: lava " << m_emit.lava.size() << ", fire "
		<< m_emit.fire.size() << ", leaves " << m_emit.leaves.size() << ", flora "
		<< m_emit.flora.size() << ", lights " << m_emit.lights.size() << std::endl;
}

/*
	Slots and spawning
*/

void ParticleSystem::push(const Spawn &s)
{
	const u32 cap = capacity();
	// a spawn may turn into a splash (rain, drip) in the same slot
	const float bound = s.life + (s.kind == ParticleKind::Rain || s.kind == ParticleKind::Drip ? 0.6f : 0.f);
	for (int k = 0; k < 64; k++) {
		const u32 i = m_cursor;
		m_cursor = (m_cursor + 1) % cap;
		if (m_death[i] > m_time)
			continue;
		m_death[i] = m_time + bound + 0.1f;
		m_high = std::max(m_high, i + 1);
		video::S3DVertexTangents v;
		const v3f rel = s.pos - intToFloat(m_anchor, 1.f);
		v.Pos = rel;
		v.Normal = s.vel;
		v.Color = s.tint;
		v.TCoords = v2f((float)i, (float)s.kind);
		v.Tangent = v3f(s.life, rnd(), s.param);
		v.Binormal = v3f(s.layer, s.size, s.glow);
		m_spawns.push_back(v);
		return;
	}
	m_dropped++; // the budget is full
}

/*
	Per frame
*/

void ParticleSystem::uploadBlock(Client *client, Camera *camera, float dtime)
{
	ParticleBlock b{};
	b.frame[0] = dtime;
	b.frame[1] = m_r.cam.time;
	b.frame[2] = (float)m_width;
	b.frame[3] = (float)m_height;
	const v3s16 shift = m_prev_anchor - m_anchor;
	b.shift[0] = shift.X;
	b.shift[1] = shift.Y;
	b.shift[2] = shift.Z;
	b.shift[3] = m_r.params.particle_wake;
	if (m_world_valid) {
		const v3s16 o = m_world_origin - m_anchor;
		b.world[0] = o.X;
		b.world[1] = o.Y;
		b.world[2] = o.Z;
		b.world[3] = 1.f;
	}
	b.anchor[0] = m_anchor.X;
	b.anchor[1] = m_anchor.Y;
	b.anchor[2] = m_anchor.Z;
	if (LocalPlayer *player = client->getEnv().getLocalPlayer()) {
		const v3f p = player->getPosition() / BS + v3f(0.f, 0.9f, 0.f) - intToFloat(m_anchor, 1.f);
		const v3f v = player->getSpeed() / BS;
		b.player[0] = p.X;
		b.player[1] = p.Y;
		b.player[2] = p.Z;
		b.player[3] = v.getLength();
		b.player_vel[0] = v.X;
		b.player_vel[1] = v.Y;
		b.player_vel[2] = v.Z;
	}
	const v3f dir = camera->getDirection();
	v3f up = camera->getCameraNode()->getUpVector();
	v3f right = up.crossProduct(dir);
	right.normalize();
	up = dir.crossProduct(right);
	up.normalize();
	const float dnr = client->getEnv().getDayNightRatio() / 1000.f;
	b.cam_right[0] = right.X;
	b.cam_right[1] = right.Y;
	b.cam_right[2] = right.Z;
	b.cam_right[3] = dnr;
	b.cam_up[0] = up.X;
	b.cam_up[1] = up.Y;
	b.cam_up[2] = up.Z;
	b.cam_up[3] = std::clamp((0.6f - dnr) / 0.4f, 0.f, 1.f);
	const v3f cp = camera->getPosition() / BS - intToFloat(m_anchor, 1.f);
	b.cam_pos[0] = cp.X;
	b.cam_pos[1] = cp.Y;
	b.cam_pos[2] = cp.Z;
	float sky[3] = {1.f, 1.f, 1.f};
	gradeSunlight(sky, client->getEnv().getTimeOfDayF());
	for (int i = 0; i < 3; i++)
		b.sky[i] = sky[i];
	b.sky[3] = m_r.params.ambient_floor;
	m_driver->setUniformBufferData(UBO_PARTICLES, &b, sizeof(b));
}

static void stateLayer(video::SMaterialLayer &l, video::ITexture *t)
{
	l.Texture = t;
	l.MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
	l.MagFilter = video::ETMAGF_NEAREST;
	l.TextureWrapU = l.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	l.AnisotropicFilter = 0;
}

void ParticleSystem::fullTargetQuad()
{
	static const video::SColor color(0, 0, 0, 255);
	static const video::S3DVertex vertices[4] = {
		video::S3DVertex(1.0, -1.0, 0.0, 0.0, 0.0, -1.0, color, 1.0, 0.0),
		video::S3DVertex(-1.0, -1.0, 0.0, 0.0, 0.0, -1.0, color, 0.0, 0.0),
		video::S3DVertex(-1.0, 1.0, 0.0, 0.0, 0.0, -1.0, color, 0.0, 1.0),
		video::S3DVertex(1.0, 1.0, 0.0, 0.0, 0.0, -1.0, color, 1.0, 1.0),
	};
	static const u16 indices[6] = {0, 1, 2, 2, 3, 0};
	m_driver->drawVertexPrimitiveList(&vertices, 4, &indices, 2);
}

void ParticleSystem::runPasses()
{
	if (!m_cleared) {
		for (auto *rt : m_rt_spawn)
			m_driver->setRenderTargetEx(rt, video::ECBF_COLOR, video::SColor(0, 0, 0, 0));
		m_cleared = true;
	}
	const int src = m_cur, dst = 1 - m_cur;
	video::SMaterial m;
	m.ZBuffer = video::ECFN_DISABLED;
	m.ZWriteEnable = video::EZW_OFF;
	m.BackfaceCulling = false;
	m.UseMipMaps = false;

	// update: every slot, src -> dst
	m.MaterialType = m_r.shsrc->getShaderInfo(m_shader_update).material;
	for (int k = 0; k < 3; k++)
		stateLayer(m.TextureLayers[k], m_state[src][k]);
	stateLayer(m.TextureLayers[3], m_static);
	stateLayer(m.TextureLayers[7], m_world);
	m_driver->setRenderTargetEx(m_rt_update[dst], 0);
	m_driver->setMaterial(m);
	fullTargetQuad();

	// spawn: one point per new particle into dst (and S3)
	if (!m_spawns.empty()) {
		video::SMaterial s;
		s.ZBuffer = video::ECFN_DISABLED;
		s.ZWriteEnable = video::EZW_OFF;
		s.BackfaceCulling = false;
		s.MaterialType = m_r.shsrc->getShaderInfo(m_shader_spawn).material;
		m_driver->setRenderTargetEx(m_rt_spawn[dst], 0);
		m_driver->setMaterial(s);
		m_driver->drawVertexPrimitiveList(m_spawns.data(), (u32)m_spawns.size(), nullptr,
				(u32)m_spawns.size(), video::EVT_TANGENTS, scene::EPT_POINTS, video::EIT_16BIT);
		m_spawns.clear();
	}
	m_cur = dst;
	m_driver->setRenderTargetEx(nullptr, 0);
}

void ParticleSystem::update(Client *client, Camera *camera, float dtime)
{
	if (!m_r.volume || !m_r.volume->hasSnapshot())
		return;
	if (!m_tables_built)
		buildTables(client);
	dtime = std::clamp(dtime, 0.f, 0.1f);
	m_dtime = dtime;
	m_time += dtime;

	m_prev_anchor = m_have_anchor ? m_anchor : camera->getOffset();
	m_anchor = camera->getOffset();
	m_have_anchor = true;
	m_cam_pos = camera->getPosition() / BS;

	// the world texture follows the light volume's snapshot (4x a second)
	m_world_timer += dtime;
	const u32 gen = m_r.volume->snapshotGeneration();
	if (gen != m_world_generation && (m_world_timer >= 0.25f || !m_world_valid)) {
		m_world_generation = gen;
		m_world_timer = 0.f;
		rebuildWorld(client);
	}
	if (!m_world_valid)
		return;

	// test mode: once the scene has settled, simulate a few seconds ahead so
	// slow particles (leaves) are on screen at the capture
	if (!m_prewarmed && test::framesSinceReady() >= 0) {
		m_prewarmed = true;
		const float secs = g_settings->exists("fx_test_particle_prewarm") ?
				g_settings->getFloat("fx_test_particle_prewarm") : 0.f;
		if (secs > 0.f) {
			rebuildWorld(client); // the settled scene, not the last throttled copy
			m_world_generation = gen;
			const float step = 1.f / 30.f;
			for (float t = 0.f; t < secs; t += step) {
				m_time += step;
				emit(client, camera, step);
				uploadBlock(client, camera, step);
				runPasses();
				m_prev_anchor = m_anchor;
			}
			infostream << "fx: GPU particles prewarmed " << secs << " s" << std::endl;
		}
	}

	emit(client, camera, dtime);
	uploadBlock(client, camera, dtime);
	m_stat_spawned += m_spawns.size();
	runPasses();
	m_stat_timer += dtime;
	if (m_stat_timer >= 2.f) {
		infostream << "fx: GPU particles: " << m_stat_spawned << " spawned in "
			<< m_stat_timer << " s, slots used " << m_high << "; emitters lava "
			<< m_emit.lava.size() << ", fire " << m_emit.fire.size() << ", leaves "
			<< m_emit.leaves.size() << ", plants " << m_emit.flora.size() << ", lights "
			<< m_emit.lights.size() << std::endl;
		m_stat_timer = 0.f;
		m_stat_spawned = 0;
	}
	if (m_dropped) {
		verbosestream << "fx: GPU particle budget full, dropped " << m_dropped << std::endl;
		m_dropped = 0;
	}
}

void ParticleSystem::draw(video::IVideoDriver *driver)
{
	if (!m_world_valid || m_high == 0)
		return;
	video::SMaterial m;
	m.MaterialType = m_r.shsrc->getShaderInfo(m_shader_draw).material;
	m.ZBuffer = video::ECFN_LESSEQUAL;
	m.ZWriteEnable = video::EZW_OFF;
	m.BackfaceCulling = false;
	// premultiplied alpha: glowing kinds write alpha 0 and simply add
	m.BlendOperation = video::EBO_ADD;
	m.BlendFactor = video::pack_textureBlendFuncSeparate(video::EBF_ONE,
			video::EBF_ONE_MINUS_SRC_ALPHA, video::EBF_ZERO, video::EBF_ONE);
	for (int k = 0; k < 3; k++)
		stateLayer(m.TextureLayers[k], m_state[m_cur][k]);
	stateLayer(m.TextureLayers[3], m_static);
	if (m_leaf_tiles) {
		auto &l = m.TextureLayers[4];
		l.Texture = m_leaf_tiles;
		l.MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
		l.MagFilter = video::ETMAGF_NEAREST;
		l.TextureWrapU = l.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	}
	if (m_r.copy.valid)
		stateLayer(m.TextureLayers[5], m_r.copy.depth);
	bindLightVolume(m);
	stateLayer(m.TextureLayers[7], m_world);

	driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);
	const u32 chunks = (m_high + CHUNK - 1) / CHUNK;
	for (u32 c = 0; c < chunks; c++) {
		// the chunk index reaches the shader through materialColor
		m.ColorParam = video::SColor(255, c & 255, (c >> 8) & 255, 0);
		driver->setMaterial(m);
		driver->drawMeshBuffer(m_quads);
	}
}

void drawParticles(video::IVideoDriver *driver)
{
	Renderer *r = renderer();
	if (r && r->particles)
		r->particles->draw(driver);
}

} // namespace fx
