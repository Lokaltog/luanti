// Luanti "fx" fork: light lists (per mesh at meshgen, per frame for shaders)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "renderer.h"

#include "client/camera.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientmap.h"
#include "client/localplayer.h"
#include "client/mapblock_mesh.h"
#include "client/node_visuals.h"
#include "itemdef.h"
#include "log.h"
#include "mapblock.h"
#include "nodedef.h"
#include <IVideoDriver.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace fx
{

void collectBlockLights(const MeshMakeData *data, std::vector<Light> &out)
{
	out.clear();
	const NodeDefManager *ndef = data->m_nodedef;
	const v3s16 base = data->m_blockpos * MAP_BLOCKSIZE;
	const s16 n = data->m_side_length;

	// Accumulate per (cluster cell, colour). Meshes are a few thousand nodes
	// at most, so a flat vector with a linear search per cell stays cheap.
	struct Acc {
		v3s16 cell;
		u32 color;
		v3f sum, lo, hi;
		u16 count;
		u8 level;
		float flicker;
	};
	std::vector<Acc> acc;

	v3s16 p;
	for (p.Z = base.Z; p.Z < base.Z + n; p.Z++)
	for (p.Y = base.Y; p.Y < base.Y + n; p.Y++)
	for (p.X = base.X; p.X < base.X + n; p.X++) {
		MapNode node = data->m_vmanip.getNodeNoExNoEmerge(p);
		const ContentFeatures &f = ndef->get(node);
		// the light volume gives every light its colour; the list only
		// carries what moves: flicker (and the held light, per frame)
		if (f.light_source == 0 || !f.visuals || f.visuals->fx_light.flicker <= 0.f)
			continue;
		const v3s16 cell(
			(p.X - base.X) / LIGHT_CLUSTER_CELL,
			(p.Y - base.Y) / LIGHT_CLUSTER_CELL,
			(p.Z - base.Z) / LIGHT_CLUSTER_CELL);
		const u32 color = f.visuals->fx_light.color.color;
		const v3f pf(p.X, p.Y, p.Z);
		auto it = std::find_if(acc.begin(), acc.end(), [&] (const Acc &a) {
			return a.cell == cell && a.color == color;
		});
		if (it == acc.end()) {
			acc.push_back({cell, color, pf, pf, pf, 1, f.light_source, f.visuals->fx_light.flicker});
		} else {
			it->sum += pf;
			it->lo = v3f(std::min(it->lo.X, pf.X), std::min(it->lo.Y, pf.Y), std::min(it->lo.Z, pf.Z));
			it->hi = v3f(std::max(it->hi.X, pf.X), std::max(it->hi.Y, pf.Y), std::max(it->hi.Z, pf.Z));
			it->count++;
			it->level = std::max(it->level, f.light_source);
			it->flicker = std::max(it->flicker, f.visuals->fx_light.flicker);
		}
	}

	out.reserve(acc.size());
	for (const Acc &a : acc) {
		Light l;
		l.pos = a.sum / (float)a.count;
		l.level = a.level;
		l.count = a.count;
		l.extent = (a.hi - a.lo).getLength() * 0.5f;
		l.color = video::SColor(a.color);
		l.flicker = a.flicker;
		out.push_back(l);
	}
}

static float hashSeed(v3s16 p)
{
	u32 h = (u32)p.X * 73856093u ^ (u32)p.Y * 19349663u ^ (u32)p.Z * 83492791u;
	return (h % 10007u) / 10007.f;
}

void Renderer::updateLights(Client *client, Camera *camera)
{
	LightsBlock &b = lights;
	b.info[0] = 0;   // count
	b.info[1] = -1;  // held index
	if (!caps.colored_light) {
		driver->setUniformBufferData(UBO_LIGHTS, &b, sizeof(b));
		return;
	}
	const Params &prm = params;
	ClientMap &map = client->getEnv().getClientMap();
	const v3f cam_pos = camera->getPosition();
	const v3f offset_bs = intToFloat(camera->getOffset(), BS);
	const v3s16 cam_node = floatToInt(cam_pos, BS);
	const float radius_nodes = std::max(16.f, prm.light_gather_radius);
	const s16 rb = (s16)std::ceil(radius_nodes / MAP_BLOCKSIZE);
	const v3s16 cam_block = getNodeBlockPos(cam_node);

	struct Candidate { float d2; const Light *l; };
	std::vector<Candidate> cands;
	// a mesh can span several blocks (mesh grid): visit each once
	std::unordered_set<const MapBlockMesh *> seen;

	v3s16 bp;
	for (bp.Z = cam_block.Z - rb; bp.Z <= cam_block.Z + rb; bp.Z++)
	for (bp.Y = cam_block.Y - rb; bp.Y <= cam_block.Y + rb; bp.Y++)
	for (bp.X = cam_block.X - rb; bp.X <= cam_block.X + rb; bp.X++) {
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		if (!block || !block->mesh || !seen.insert(block->mesh).second)
			continue;
		for (const Light &l : block->mesh->getFxLights()) {
			const float d2 = (l.pos * BS - cam_pos).getLengthSQ();
			if (d2 <= radius_nodes * radius_nodes * BS * BS)
				cands.push_back({d2, &l});
		}
	}

	const int max_world = MAX_LIGHTS - 1; // one slot for the held light
	if ((int)cands.size() > max_world) {
		std::nth_element(cands.begin(), cands.begin() + max_world, cands.end(),
				[] (const Candidate &a, const Candidate &c) { return a.d2 < c.d2; });
		cands.resize(max_world);
	}

	auto push = [&] (v3f pos_bs, float radius_bs, video::SColor c, float flicker, float seed,
			float strength) {
		const int i = b.info[0]++;
		b.pos_radius[i * 4 + 0] = pos_bs.X - offset_bs.X;
		b.pos_radius[i * 4 + 1] = pos_bs.Y - offset_bs.Y;
		b.pos_radius[i * 4 + 2] = pos_bs.Z - offset_bs.Z;
		b.pos_radius[i * 4 + 3] = radius_bs;
		// strength weights the hue mix; the shader normalizes the result
		b.color_seed[i * 4 + 0] = c.getRed() / 255.f * strength;
		b.color_seed[i * 4 + 1] = c.getGreen() / 255.f * strength;
		b.color_seed[i * 4 + 2] = c.getBlue() / 255.f * strength;
		// integer part: flicker amplitude in 1/100ths, fraction: seed
		b.color_seed[i * 4 + 3] =
				std::floor(std::clamp(flicker * prm.flicker, 0.f, 0.99f) * 100.f) + seed * 0.99f;
	};

	for (const auto &c : cands) {
		const Light &l = *c.l;
		// A cluster of N emitters weighs like 1 + log2(N)/2 lights: a lava
		// lake should tint the cave, not drown the torch next to you.
		push(l.pos * BS, (l.level + 1.5f + l.extent) * BS, l.color, l.flicker,
				hashSeed(v3s16(l.pos.X, l.pos.Y, l.pos.Z)), 1.f + 0.5f * std::log2((float)l.count));
	}

	// held light: a torch in hand lights the camera's surroundings
	u8 held_level = 0;
	if (prm.held_light) {
		LocalPlayer *player = client->getEnv().getLocalPlayer();
		ItemStack selected, hand;
		const ItemStack &item = player->getWieldedItem(&selected, &hand);
		content_t id;
		if (!item.empty() && client->ndef()->getId(item.name, id)) {
			const ContentFeatures &f = client->ndef()->get(id);
			if (f.light_source > 0 && f.visuals) {
				// a little dimmer than the placed node
				held_level = f.light_source > 1 ? f.light_source - 1 : 1;
				b.info[1] = b.info[0];
				push(cam_pos + camera->getDirection() * (0.6f * BS),
						(f.light_source + 1.5f) * BS * 0.85f, f.visuals->fx_light.color,
						std::max(f.visuals->fx_light.flicker, 0.6f), 0.5f, 1.f);
			}
		}
	}
	if (volume)
		volume->setHeld(cam_node, held_level);
	driver->setUniformBufferData(UBO_LIGHTS, &b, sizeof(b));
}

} // namespace fx
