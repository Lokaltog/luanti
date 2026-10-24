// Luanti "afx" fork: environmental audio, main-thread side
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "afx.h"

#include "client/client.h"
#include "client/clientmap.h"
#include "client/sound.h"
#include "light.h"
#include "mapblock.h"
#include "nodedef.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/tracy_wrapper.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace afx
{

/*
	Settings
*/

static Params s_params;

static const char *const SETTING_NAMES[] = {
	"afx_enable", "afx_probe_interval", "afx_probe_radius", "afx_rays",
	"afx_reverb", "afx_max_decay", "afx_echo", "afx_occlusion",
	"afx_outdoor_muffle", "afx_smoothing", "afx_debug",
	"afx_dry_sounds", "afx_outdoor_sounds", "afx_indoor_sounds",
};

static void readParams()
{
	Params p;
	auto *s = g_settings;
	p.enabled = s->getBool("afx_enable");
	p.probe_interval = rangelim(s->getFloat("afx_probe_interval"), 0.05f, 5.f);
	p.probe_radius = rangelim(s->getS16("afx_probe_radius"), 8, 64);
	p.rays = rangelim(s->getS32("afx_rays"), 16, 512);
	p.reverb = rangelim(s->getFloat("afx_reverb"), 0.f, 2.f);
	p.max_decay = rangelim(s->getFloat("afx_max_decay"), 0.2f, 20.f);
	p.echo = rangelim(s->getFloat("afx_echo"), 0.f, 2.f);
	p.occlusion = rangelim(s->getFloat("afx_occlusion"), 0.f, 2.f);
	p.outdoor_muffle = rangelim(s->getFloat("afx_outdoor_muffle"), 0.f, 4.f);
	p.smoothing = rangelim(s->getFloat("afx_smoothing"), 0.05f, 10.f);
	p.debug = s->getBool("afx_debug");
	p.dry_sounds = s->get("afx_dry_sounds");
	p.outdoor_sounds = s->get("afx_outdoor_sounds");
	p.indoor_sounds = s->get("afx_indoor_sounds");
	s_params = p;
}

static void onSettingChanged(const std::string &, void *)
{
	readParams();
}

void init()
{
	readParams();
	for (auto name : SETTING_NAMES)
		g_settings->registerChangedCallback(name, &onSettingChanged, &s_params);
}

void deinit()
{
	g_settings->deregisterAllChangedCallbacks(&s_params);
}

const Params &params() { return s_params; }

/*
	Materials
*/

static bool nameHas(const std::string &name, std::initializer_list<const char *> parts)
{
	for (const char *p : parts)
		if (name.find(p) != std::string::npos)
			return true;
	return false;
}

Material classifyNode(const ContentFeatures &f)
{
	auto group = [&](const char *g) { return itemgroup_get(f.groups, g) != 0; };
	const std::string &name = f.name;

	if (f.name.empty() || f.name == "air" || f.name == "ignore")
		return Material::Air;

	if (f.liquid_type != LIQUID_NONE) {
		if (group("lava") || nameHas(name, {"lava"}) ||
				(f.damage_per_second > 0 && f.light_source > 0))
			return Material::Lava;
		return Material::Water;
	}

	switch (f.drawtype) {
	case NDT_AIRLIKE:
	case NDT_TORCHLIKE:
	case NDT_SIGNLIKE:
	case NDT_PLANTLIKE:
	case NDT_FIRELIKE:
	case NDT_RAILLIKE:
		return Material::Air;
	default:
		break;
	}
	if (!f.walkable)
		return Material::Air; // tall grass, vines, ladders, cobwebs

	if (group("leaves") || group("leafdecay") || nameHas(name, {"leaves", "leaf"}))
		return Material::Foliage;
	if (group("wool") || group("carpet") || group("bed") || nameHas(name, {"wool", "carpet", "hay"}))
		return Material::Soft;
	if (group("snow") || group("snow_cover") || nameHas(name, {"snow"}))
		return Material::Snow;
	if (group("glass") || f.drawtype == NDT_GLASSLIKE || f.drawtype == NDT_GLASSLIKE_FRAMED ||
			f.drawtype == NDT_GLASSLIKE_FRAMED_OPTIONAL || nameHas(name, {"glass", "ice"}))
		return Material::Glass;

	// Holey shapes let sound through. Doors, panes and trapdoors are not
	// here: their collision boxes decide (see thinCode), open or shut.
	if (f.drawtype == NDT_FENCELIKE ||
			nameHas(name, {"fence", "bars", "ladder", "lantern", "chain", "scaffold", "rail"}))
		return Material::Leaky;

	if (group("wood") || group("tree") || group("planks") || group("axey") ||
			group("choppy") || nameHas(name, {"wood", "plank", "log"}))
		return Material::Wood;
	if (group("dirt") || group("sand") || group("soil") || group("gravel") ||
			group("crumbly") || group("shovely") || group("falling_node"))
		return Material::Earth;
	if (group("stone") || group("cracky") || group("pickaxey") || group("ore") ||
			group("metal"))
		return Material::Stone;
	return Material::Earth;
}

namespace {
struct MaterialTable {
	const NodeDefManager *ndef = nullptr;
	u32 size = 0;
	std::vector<u8> mat;
	std::vector<bool> shaped; // needs its collision boxes looked at
	std::unordered_map<u32, u8> thin; // (content << 8 | param2) -> cell bits

	void refresh(const NodeDefManager *n)
	{
		if (n == ndef && n->size() == size)
			return;
		ndef = n;
		size = n->size();
		mat.assign(size, (u8)Material::Air);
		shaped.assign(size, false);
		thin.clear();
		for (u32 c = 0; c < size; c++) {
			const ContentFeatures &f = n->get((content_t)c);
			Material m = classifyNode(f);
			mat[c] = (u8)m;
			const bool boxy = f.drawtype == NDT_NODEBOX || f.drawtype == NDT_MESH ||
					!f.collision_box.fixed.empty();
			shaped[c] = boxy && f.walkable && m != Material::Air && m != Material::Leaky &&
					m != Material::Water && m != Material::Lava;
		}
	}

	Material operator()(content_t c) const
	{
		return c < mat.size() ? (Material)mat[c] : Material::Air;
	}
};
MaterialTable s_materials;

/// Is the node's footprint, seen along `axis`, mostly covered? Samples a
/// 5x5 grid over the face (boxes are in BS units, centred on the node).
bool coversAxis(const std::vector<aabb3f> &boxes, int axis)
{
	const int u = (axis + 1) % 3, v = (axis + 2) % 3;
	int covered = 0;
	for (int i = 0; i < 5; i++)
	for (int j = 0; j < 5; j++) {
		float pu = (-0.4f + 0.2f * i) * BS, pv = (-0.4f + 0.2f * j) * BS;
		for (const auto &b : boxes) {
			const float mn[3] = {b.MinEdge.X, b.MinEdge.Y, b.MinEdge.Z};
			const float mx[3] = {b.MaxEdge.X, b.MaxEdge.Y, b.MaxEdge.Z};
			if (pu >= mn[u] && pu <= mx[u] && pv >= mn[v] && pv <= mx[v]) {
				covered++;
				break;
			}
		}
	}
	return covered >= 18; // ~70 %
}

/// Cell bits for a shaped node: full, thin along one axis, or passable.
u8 shapeCell(const NodeDefManager *ndef, MapNode n, Material m)
{
	const ContentFeatures &f = ndef->get(n);
	// Connected nodeboxes (panes, walls) depend on neighbours; a pane in a
	// window is a wall either way.
	if (f.collision_box.type == NODEBOX_CONNECTED || f.node_box.type == NODEBOX_CONNECTED)
		return (u8)m;
	std::vector<aabb3f> boxes;
	n.getCollisionBoxes(ndef, &boxes);
	int axes = 0, last = -1;
	for (int a = 0; a < 3; a++) {
		if (coversAxis(boxes, a)) {
			axes++;
			last = a;
		}
	}
	if (axes == 0)
		return (u8)Material::Leaky; // posts, lanterns: sound goes round
	if (axes == 1)
		return makeThin(m, last);   // door, pane, trapdoor, slab
	return (u8)m;
}
}

/*
	Snapshot
*/

std::shared_ptr<VoxelSnapshot> buildSnapshot(Client *client, v3s16 center, s16 radius)
{
	ZoneScoped;
	const NodeDefManager *ndef = client->getNodeDefManager();
	s_materials.refresh(ndef);
	ClientMap &map = client->getEnv().getClientMap();

	auto snap = std::make_shared<VoxelSnapshot>();
	snap->size = radius * 2;
	snap->origin = center - v3s16(radius, radius, radius);
	const s16 size = snap->size;
	snap->cells.assign((size_t)size * size * size, (u8)CELL_UNLOADED);
	thread_local std::vector<bool> sunlit;
	sunlit.assign(snap->cells.size(), false);

	const v3s16 lo = snap->origin;
	const v3s16 hi = lo + v3s16(size - 1, size - 1, size - 1);
	const v3s16 blo = getContainerPos(lo, MAP_BLOCKSIZE);
	const v3s16 bhi = getContainerPos(hi, MAP_BLOCKSIZE);

	for (s16 bz = blo.Z; bz <= bhi.Z; bz++)
	for (s16 by = blo.Y; by <= bhi.Y; by++)
	for (s16 bx = blo.X; bx <= bhi.X; bx++) {
		v3s16 bp(bx, by, bz);
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		if (!block)
			continue; // stays CELL_UNLOADED
		const v3s16 base = bp * MAP_BLOCKSIZE;
		const v3s16 a(std::max(lo.X, base.X), std::max(lo.Y, base.Y), std::max(lo.Z, base.Z));
		const v3s16 b(std::min<s16>(hi.X, base.X + MAP_BLOCKSIZE - 1),
				std::min<s16>(hi.Y, base.Y + MAP_BLOCKSIZE - 1),
				std::min<s16>(hi.Z, base.Z + MAP_BLOCKSIZE - 1));
		for (s16 z = a.Z; z <= b.Z; z++)
		for (s16 y = a.Y; y <= b.Y; y++) {
			u32 i = snap->index(v3s16(a.X, y, z));
			for (s16 x = a.X; x <= b.X; x++, i++) {
				MapNode n = block->getNodeNoCheck(x - base.X, y - base.Y, z - base.Z);
				content_t c = n.getContent();
				if (c == CONTENT_IGNORE)
					continue;
				Material m = s_materials(c);
				u8 cell = (u8)m;
				if (s_materials.shaped[c]) {
					u32 key = ((u32)c << 8) | n.getParam2();
					auto it = s_materials.thin.find(key);
					if (it == s_materials.thin.end())
						it = s_materials.thin.emplace(key, shapeCell(ndef, n, m)).first;
					cell = it->second;
				}
				if (m == Material::Air &&
						n.getLight(LIGHTBANK_DAY, ndef->getLightingFlags(c)) == LIGHT_SUN)
					sunlit[i] = true;
				snap->cells[i] = cell;
			}
		}
	}

	// Sky: sunlit air with nothing that closes the column above it. Leaves
	// and fences don't close it (a forest floor is outdoors); a glass roof
	// does (a greenhouse is indoors, even though the sun shines in).
	for (s16 z = 0; z < size; z++)
	for (s16 x = 0; x < size; x++) {
		bool open = true;
		for (s16 y = size - 1; y >= 0; y--) {
			u8 &cell = snap->cells[snap->index(lo + v3s16(x, y, z))];
			if (cell & CELL_UNLOADED)
				continue;
			Material m = cellMaterial(cell);
			if (m == Material::Air) {
				if (open && sunlit[snap->index(lo + v3s16(x, y, z))])
					cell |= CELL_SKY;
			} else if (m != Material::Foliage && m != Material::Leaky &&
					cellBlocks(cell, 1)) {
				open = false; // a roof, a floor, a closed trapdoor
			}
		}
	}
	return snap;
}

/*
	Per frame
*/

static float s_timer = 0.f;
static float s_build_ms = 0.f;
static bool s_was_enabled = true;

void update(Client *client, ISoundManager *sound, v3f ear, float dtime)
{
	if (!sound)
		return;
	const Params &p = s_params;
	if (!p.enabled) {
		if (s_was_enabled)
			sound->setAcoustics(nullptr, p); // tells the sound thread to detach
		s_was_enabled = false;
		return;
	}
	s_was_enabled = true;

	// With afx_debug, log the state every 2 s (for headless runs and tuning)
	static float log_timer = 0.f;
	if (p.debug && (log_timer -= dtime) <= 0.f) {
		log_timer = 2.f;
		std::string line = debugLine(sound);
		if (!line.empty())
			infostream << line << " | ear " << ear.X << "," << ear.Y << "," << ear.Z << std::endl;
	}

	s_timer -= dtime;
	if (s_timer > 0.f)
		return;
	s_timer = p.probe_interval;

	auto t0 = std::chrono::steady_clock::now();
	v3s16 center = floatToInt(ear * BS, BS);
	std::shared_ptr<const VoxelSnapshot> snap = buildSnapshot(client, center, p.probe_radius);
	s_build_ms = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	sound->setAcoustics(std::move(snap), p);
}

std::string debugLine(ISoundManager *sound)
{
	if (!s_params.debug || !sound)
		return "";
	Report r;
	if (!sound->getAcousticReport(r))
		return s_params.enabled ? "afx: waiting for first probe" : "afx: off";
	const Acoustics &a = r.now;
	std::ostringstream os;
	os << std::fixed << std::setprecision(2)
		<< "afx: encl " << a.enclosure
		<< " | wet " << a.wet << " rt60 " << std::setprecision(1) << a.decay << "s"
		<< std::setprecision(2) << " hf " << a.decay_hf_ratio
		<< " | echo " << a.echo;
	if (a.echo > 0.05f)
		os << " @" << (int)(a.echo_delay * 1000.f) << "ms";
	os << " | out " << a.outdoor_gain << " hf " << a.outdoor_hf << " wall " << a.outdoor_wall
		<< " path " << std::setprecision(0) << a.outdoor_path
		<< std::setprecision(2) << " focus " << a.outdoor_focus;
	if (a.submerged > 0.01f)
		os << " | submerged " << a.submerged;
	os << " | src " << r.sources << " occl " << r.occluded
		<< std::setprecision(1) << " | probe " << r.probe_ms << "ms snap " << s_build_ms << "ms";
	return os.str();
}

} // namespace afx
