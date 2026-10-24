// Luanti "fx" fork: GPU particle emitters (CPU side)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Decides what spawns where, per frame, from the emitter lists the world
// rebuild found in the light volume's node snapshot (lava and fire, leaves
// with air below, plants under the open sky, light sources) and from the
// weather and world the fx_hints mod describes. Rates are per second; the
// fractional part carries over (m_acc), so low rates still spawn.

#include "particles.h"
#include "renderer.h"

#include "client/camera.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientmap.h"
#include "client/node_visuals.h"
#include "nodedef.h"
#include <algorithm>
#include <cmath>

namespace fx
{

namespace
{

// Nether air by biome (Mineclonia names, as fx_ambience's NETHER_AIR)
struct NetherAir {
	const char *biome;
	video::SColor color;
	float glow, rate, rise, size;
};
const NetherAir NETHER_AIR[] = {
	{"CrimsonForest", video::SColor(255, 255, 58, 48), 0.6f, 14.f, 0.05f, 0.035f},  // spores
	{"WarpedForest", video::SColor(255, 64, 224, 208), 0.6f, 14.f, 0.05f, 0.035f},
	{"SoulsandValley", video::SColor(255, 159, 180, 200), 0.f, 18.f, -0.08f, 0.04f}, // ash
	{"BasaltDelta", video::SColor(255, 232, 228, 224), 0.f, 26.f, -0.18f, 0.045f},
	{"", video::SColor(255, 255, 128, 48), 1.0f, 10.f, 0.35f, 0.03f},               // wastes: cinders
};

enum Acc { A_LAVA, A_FIRE, A_LEAF, A_RAIN, A_SNOW, A_DRIP, A_LIGHTDUST, A_DAYDUST,
	A_ASH, A_FIREFLY, A_BURST, A_FLAME, A_FLAME_POP };

} // namespace

void ParticleSystem::emit(Client *client, Camera *camera, float dtime)
{
	const Params &p = m_r.params;
	const auto &ev = m_r.events;
	const Hints &hints = m_r.hints;
	const float dens = p.particle_density;
	if (dens <= 0.f)
		return;
	const float dnr = client->getEnv().getDayNightRatio() / 1000.f;
	const float night = std::clamp((0.6f - dnr) / 0.4f, 0.f, 1.f);
	const v3f cam = m_cam_pos;
	ClientMap &map = client->getEnv().getClientMap();
	const NodeDefManager *ndef = client->ndef();
	constexpr s16 SX = LightVolume::SX, SY = LightVolume::SY, SZ = LightVolume::SZ;

	// how many to spawn this frame for a rate per second
	auto count = [&] (int acc, float rate) -> int {
		m_acc[acc] += std::max(rate, 0.f) * dtime;
		const int n = (int)m_acc[acc];
		m_acc[acc] -= n;
		return std::min(n, 2048);
	};
	auto pick = [&] (const std::vector<v3s16> &l) -> v3s16 {
		return l[std::min<size_t>((size_t)(rnd() * l.size()), l.size() - 1)];
	};
	// window voxel flags at a world node (0 outside)
	auto voxel = [&] (v3s16 n) -> u8 {
		const v3s16 v = n - m_world_origin;
		if (v.X < 0 || v.Y < 0 || v.Z < 0 || v.X >= SX || v.Y >= SY || v.Z >= SZ)
			return 0;
		const u32 tx = (v.Y % LightVolume::ATLAS_COLS) * SX + v.X;
		const u32 ty = (v.Y / LightVolume::ATLAS_COLS) * SZ + v.Z;
		const video::SColor c(reinterpret_cast<const u32 *>(m_world_px.data())[ty * LightVolume::ATLAS_W + tx]);
		return (c.getRed() ? 1 : 0) | (c.getBlue() ? 2 : 0) | (c.getGreen() ? 4 : 0);
	};
	// highest rain-blocking node of a column (world y), or INT16_MIN
	auto columnTop = [&] (s16 x, s16 z) -> int {
		const s16 vx = x - m_world_origin.X, vz = z - m_world_origin.Z;
		if (vx < 0 || vz < 0 || vx >= SX || vz >= SZ)
			return -32768;
		const s16 t = m_column_top[(size_t)vz * SX + vx];
		return t < 0 ? -32768 : m_world_origin.Y + t;
	};
	const float wind_x = std::cos(ev.wind_angle) * ev.wind, wind_z = std::sin(ev.wind_angle) * ev.wind;

	/*
		1. Sparks and embers: lava surfaces, fire
	*/
	const float sparks = p.particle_sparks * dens;
	if (sparks > 0.f && !m_emit.lava.empty()) {
		const int n = count(A_LAVA, m_emit.lava.size() * 1.5f * sparks);
		for (int i = 0; i < n; i++) {
			const v3s16 c = pick(m_emit.lava);
			Spawn s;
			s.kind = ParticleKind::Spark;
			s.pos = v3f(c.X + rnd(-0.45f, 0.45f), c.Y + 0.45f, c.Z + rnd(-0.45f, 0.45f));
			s.vel = v3f(rnd(-0.5f, 0.5f), rnd(1.2f, 3.2f), rnd(-0.5f, 0.5f));
			s.life = rnd(0.9f, 2.4f);
			s.size = rnd(0.04f, 0.07f);
			s.glow = rnd(0.7f, 1.2f);
			push(s);
		}
		// now and then the lava pops: a burst of fast sparks
		const int bursts = count(A_BURST, m_emit.lava.size() * 0.012f * sparks);
		for (int b = 0; b < bursts; b++) {
			const v3s16 c = pick(m_emit.lava);
			const v3f at(c.X + rnd(-0.3f, 0.3f), c.Y + 0.45f, c.Z + rnd(-0.3f, 0.3f));
			const int k = 6 + (int)(rnd() * 10);
			for (int i = 0; i < k; i++) {
				Spawn s;
				s.kind = ParticleKind::Spark;
				s.pos = at;
				const float a = rnd(0.f, 6.2832f), h = rnd(0.2f, 1.6f);
				s.vel = v3f(std::cos(a) * h, rnd(2.5f, 5.5f), std::sin(a) * h);
				s.life = rnd(1.0f, 2.2f);
				s.size = rnd(0.035f, 0.055f);
				s.glow = rnd(1.0f, 1.6f);
				push(s);
			}
		}
	}
	if (sparks > 0.f && !m_emit.fire.empty()) {
		const int n = count(A_FIRE, m_emit.fire.size() * 4.f * sparks);
		for (int i = 0; i < n; i++) {
			const v3s16 c = pick(m_emit.fire);
			Spawn s;
			s.kind = ParticleKind::Spark;
			s.pos = v3f(c.X + rnd(-0.35f, 0.35f), c.Y + rnd(-0.3f, 0.3f), c.Z + rnd(-0.35f, 0.35f));
			s.vel = v3f(rnd(-0.4f, 0.4f), rnd(1.0f, 2.6f), rnd(-0.4f, 0.4f));
			s.life = rnd(0.6f, 1.7f);
			s.size = rnd(0.035f, 0.06f);
			s.glow = rnd(0.8f, 1.3f);
			push(s);
		}
	}

	// open flames (torches, candles, campfires): a spark now and then, rising
	// in the flame's heat, falling and bouncing once it cools; sometimes a
	// little pop of several
	if (sparks > 0.f && !m_emit.flames.empty()) {
		auto flame_spark = [&] (const v3f &at, float speed) {
			Spawn s;
			s.kind = ParticleKind::Spark;
			s.pos = at;
			const float a = rnd(0.f, 6.2832f), h = rnd(0.05f, 0.5f) * speed;
			s.vel = v3f(std::cos(a) * h, rnd(0.6f, 1.6f) * speed, std::sin(a) * h);
			s.life = rnd(1.0f, 2.2f); // long enough to cool, fall and bounce
			s.size = rnd(0.02f, 0.035f);
			s.glow = rnd(0.6f, 1.0f);
			push(s);
		};
		const int n = count(A_FLAME, m_emit.flames.size() * 0.7f * sparks);
		for (int i = 0; i < n; i++) {
			const v3s16 c = pick(m_emit.flames);
			flame_spark(v3f(c.X + rnd(-0.06f, 0.06f), c.Y + rnd(0.05f, 0.2f), c.Z + rnd(-0.06f, 0.06f)), 1.f);
		}
		const int pops = count(A_FLAME_POP, m_emit.flames.size() * 0.04f * sparks);
		for (int b = 0; b < pops; b++) {
			const v3s16 c = pick(m_emit.flames);
			const v3f at(c.X + rnd(-0.05f, 0.05f), c.Y + 0.15f, c.Z + rnd(-0.05f, 0.05f));
			const int k = 3 + (int)(rnd() * 5);
			for (int i = 0; i < k; i++)
				flame_spark(at, 1.8f);
		}
	}

	/*
		2. Leaves: from leaf blocks with air below; more in gusty weather
	*/
	const float leaves = p.particle_leaves * dens;
	if (leaves > 0.f && !m_emit.leaves.empty()) {
		const float gust = 1.f + std::max(ev.wind - 0.5f, 0.f) * 2.f;
		const int n = count(A_LEAF, m_emit.leaves.size() * 0.06f * leaves * gust);
		for (int i = 0; i < n; i++) {
			const v3s16 c = pick(m_emit.leaves);
			const MapNode node = map.getNode(c);
			const ContentFeatures &f = ndef->get(node);
			const content_t id = node.getContent();
			if (id >= m_info.size() || m_info[id].leaf_layer < 0)
				continue; // the block changed since the scan
			Spawn s;
			s.kind = ParticleKind::Leaf;
			s.pos = v3f(c.X + rnd(-0.4f, 0.4f), c.Y - 0.56f, c.Z + rnd(-0.4f, 0.4f));
			s.vel = v3f(rnd(-0.2f, 0.2f) + wind_x * 0.3f, rnd(-0.4f, -0.1f), rnd(-0.2f, 0.2f) + wind_z * 0.3f);
			s.life = rnd(28.f, 40.f);
			s.param = rnd(-1.f, 1.f); // spin
			s.size = rnd(0.05f, 0.08f); // one leaf, not a block face
			s.layer = m_info[id].leaf_layer;
			s.tint = f.visuals ? f.visuals->getColor(f, node.param2) : video::SColor(0xffffffff);
			push(s);
		}
	}

	/*
		3. Weather: rain (splashes on the GPU), snow, drips off leaves
	*/
	const float weather = p.particle_weather * dens;
	const bool wet = hints.weather == "rain" || hints.weather == "thunder";
	const bool snowing = hints.weather == "snow" || (wet && ev.cold > 0.5f);
	const bool overworld = ev.overworld > 0.5f;
	if (weather > 0.f && overworld && (wet || snowing)) {
		const float storm = 1.f + ev.storm;
		if (!snowing) {
			const float intensity = ev.raining * storm * weather;
			const int n = count(A_RAIN, 2400.f * intensity);
			for (int i = 0; i < n; i++) {
				const float a = rnd(0.f, 6.2832f), r = std::sqrt(rnd()) * 22.f;
				const v3f at(cam.X + std::cos(a) * r, cam.Y + rnd(8.f, 16.f), cam.Z + std::sin(a) * r);
				if (columnTop((s16)std::floor(at.X + 0.5f), (s16)std::floor(at.Z + 0.5f)) >= at.Y - 0.5f)
					continue; // under a roof
				Spawn s;
				s.kind = ParticleKind::Rain;
				s.pos = at;
				s.vel = v3f(wind_x * 1.2f, -rnd(12.f, 15.f), wind_z * 1.2f);
				s.life = 3.f;
				s.size = rnd(0.018f, 0.028f);
				push(s);
			}
		} else {
			const float intensity = std::max(ev.raining, wet ? 1.f : 0.6f) * weather;
			const int n = count(A_SNOW, 450.f * intensity * storm);
			for (int i = 0; i < n; i++) {
				const float a = rnd(0.f, 6.2832f), r = std::sqrt(rnd()) * 24.f;
				const v3f at(cam.X + std::cos(a) * r, cam.Y + rnd(4.f, 14.f), cam.Z + std::sin(a) * r);
				if (columnTop((s16)std::floor(at.X + 0.5f), (s16)std::floor(at.Z + 0.5f)) >= at.Y - 0.5f)
					continue;
				Spawn s;
				s.kind = ParticleKind::Snow;
				s.pos = at;
				s.vel = v3f(wind_x * 0.6f, -rnd(0.7f, 1.3f), wind_z * 0.6f);
				s.life = rnd(12.f, 18.f);
				s.size = rnd(0.03f, 0.055f);
				s.param = rnd(0.f, 6.2832f);
				push(s);
			}
		}
		if (wet && !snowing && !m_emit.canopy_leaves.empty()) {
			const int n = count(A_DRIP, m_emit.canopy_leaves.size() * 0.5f * ev.raining * weather);
			for (int i = 0; i < n; i++) {
				const v3s16 c = pick(m_emit.canopy_leaves);
				Spawn s;
				s.kind = ParticleKind::Drip;
				s.pos = v3f(c.X + rnd(-0.42f, 0.42f), c.Y - 0.53f, c.Z + rnd(-0.42f, 0.42f));
				s.vel = v3f(0.f, -rnd(0.2f, 0.8f), 0.f);
				s.life = 3.f;
				s.size = rnd(0.014f, 0.02f);
				push(s);
			}
		}
	}

	/*
		4. Dust, ash, fireflies
	*/
	const float motes = p.particle_motes * dens;
	if (motes > 0.f) {
		// dust around light sources
		if (!m_emit.lights.empty()) {
			const int n = count(A_LIGHTDUST, m_emit.lights.size() * 0.6f * motes);
			for (int i = 0; i < n; i++) {
				const v3s16 c = pick(m_emit.lights);
				const v3f at(c.X + rnd(-2.5f, 2.5f), c.Y + rnd(-1.5f, 2.5f), c.Z + rnd(-2.5f, 2.5f));
				if (voxel(floatToInt(at, 1.f)) & 3)
					continue; // inside a block or liquid
				Spawn s;
				s.kind = ParticleKind::Dust;
				s.pos = at;
				s.vel = v3f(rnd(-0.05f, 0.05f), rnd(-0.03f, 0.05f), rnd(-0.05f, 0.05f));
				s.life = rnd(5.f, 9.f);
				s.size = rnd(0.02f, 0.04f);
				s.tint = video::SColor(255, 255, 240, 208);
				push(s);
			}
		}
		// daylight dust: drifting in lit air around the camera
		if (overworld && !wet && !snowing && dnr > 0.3f) {
			const int n = count(A_DAYDUST, 45.f * motes * std::min(dnr, 1.f));
			for (int i = 0; i < n; i++) {
				const v3f at(cam.X + rnd(-10.f, 10.f), cam.Y + rnd(-3.f, 5.f), cam.Z + rnd(-10.f, 10.f));
				if (voxel(floatToInt(at, 1.f)) & 3)
					continue;
				Spawn s;
				s.kind = ParticleKind::Dust;
				s.pos = at;
				s.vel = v3f(rnd(-0.05f, 0.05f), rnd(-0.02f, 0.03f), rnd(-0.05f, 0.05f));
				s.life = rnd(6.f, 10.f);
				s.size = rnd(0.018f, 0.032f);
				s.tint = video::SColor(255, 255, 248, 230);
				push(s);
			}
		}
		// Nether ash, spores and cinders by biome
		if (ev.nether > 0.5f) {
			const NetherAir *air = &NETHER_AIR[4];
			for (const auto &a : NETHER_AIR)
				if (a.biome[0] && hints.biome == a.biome)
					air = &a;
			const int n = count(A_ASH, air->rate * motes * 2.f);
			for (int i = 0; i < n; i++) {
				const v3f at(cam.X + rnd(-14.f, 14.f), cam.Y + rnd(-4.f, 8.f), cam.Z + rnd(-14.f, 14.f));
				if (voxel(floatToInt(at, 1.f)) & 3)
					continue;
				Spawn s;
				s.kind = ParticleKind::Ash;
				s.pos = at;
				s.vel = v3f(rnd(-0.3f, 0.3f), air->rise + rnd(-0.1f, 0.1f), rnd(-0.3f, 0.3f));
				s.life = rnd(5.f, 9.f);
				s.param = air->rise;
				s.size = air->size * rnd(1.0f, 1.8f);
				s.tint = air->color;
				s.glow = air->glow;
				push(s);
			}
		}
		// fireflies: night, over plants under the open sky, not in rain
		if (night > 0.5f && !wet && !snowing && !m_emit.flora.empty()) {
			const int n = count(A_FIREFLY, std::min<float>(m_emit.flora.size(), 400.f) * 0.1f * motes);
			for (int i = 0; i < n; i++) {
				const v3s16 c = pick(m_emit.flora);
				Spawn s;
				s.kind = ParticleKind::Firefly;
				s.pos = v3f(c.X + rnd(-0.6f, 0.6f), c.Y + rnd(0.2f, 1.6f), c.Z + rnd(-0.6f, 0.6f));
				s.vel = v3f(rnd(-0.2f, 0.2f), rnd(-0.05f, 0.1f), rnd(-0.2f, 0.2f));
				s.life = rnd(5.f, 9.f);
				s.size = rnd(0.035f, 0.05f);
				s.tint = video::SColor(255, 200, 255, 112);
				s.glow = rnd(0.15f, 0.3f);
				push(s);
			}
		}
	}
	(void)camera;
}

} // namespace fx
