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

	// Thin or holey shapes let sound through: fences, panes, bars, doors.
	if (f.drawtype == NDT_FENCELIKE ||
			nameHas(name, {"fence", "pane", "bars", "door", "gate", "ladder", "lantern",
					"chain", "scaffold", "rail"}))
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

	void refresh(const NodeDefManager *n)
	{
		if (n == ndef && n->size() == size)
			return;
		ndef = n;
		size = n->size();
		mat.assign(size, (u8)Material::Air);
		for (u32 c = 0; c < size; c++)
			mat[c] = (u8)classifyNode(n->get((content_t)c));
	}

	Material operator()(content_t c) const
	{
		return c < mat.size() ? (Material)mat[c] : Material::Air;
	}
};
MaterialTable s_materials;
}

/*
	Snapshot
*/

static constexpr u8 TMP_SUNLIT = 1 << 6;

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
				if (m == Material::Air &&
						n.getLight(LIGHTBANK_DAY, ndef->getLightingFlags(c)) == LIGHT_SUN)
					cell |= TMP_SUNLIT;
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
				if (open && (cell & TMP_SUNLIT))
					cell |= CELL_SKY;
			} else if (m != Material::Foliage && m != Material::Leaky) {
				open = false;
			}
			cell &= ~TMP_SUNLIT;
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
	os << " | out " << a.outdoor_gain << " hf " << a.outdoor_hf
		<< " path " << std::setprecision(0) << a.outdoor_path
		<< std::setprecision(2) << " focus " << a.outdoor_focus;
	if (a.submerged > 0.01f)
		os << " | submerged " << a.submerged;
	os << " | src " << r.sources << " occl " << r.occluded
		<< std::setprecision(1) << " | probe " << r.probe_ms << "ms snap " << s_build_ms << "ms";
	return os.str();
}

} // namespace afx
