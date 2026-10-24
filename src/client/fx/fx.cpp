// Luanti "fx" fork: mood renderer, milestone 1 (light & glow)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "fx.h"

#include "client/client.h"
#include "client/camera.h"
#include "client/clientmap.h"
#include "client/mapblock_mesh.h"
#include "client/node_visuals.h"
#include "client/shader.h"
#include "client/texturepaths.h"
#include "client/texturesource.h"
#include "client/localplayer.h"
#include "client/clientenvironment.h"
#include "log.h"
#include "mapblock.h"
#include "nodedef.h"
#include "itemdef.h"
#include "porting.h"
#include "filesys.h"
#include "settings.h"
#include "util/string.h"
#include "constants.h"
#include <IImage.h>
#include <IMaterialRendererServices.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace fx
{

/*
	Params
*/

static Params s_params;

static const char *const SETTING_NAMES[] = {
	"fx_enable", "fx_hdr_composite", "fx_tonemapper", "fx_exposure",
	"fx_grade_saturation", "fx_grade_contrast",
	"fx_bloom_threshold", "fx_bloom_knee", "fx_bloom_intensity",
	"fx_emissive_strength", "fx_auto_emissive_threshold",
	"fx_colored_light", "fx_light_saturation", "fx_light_gather_radius",
	"fx_flicker", "fx_held_light", "fx_debug_view", "fx_shader_hot_reload",
};

static void readParams()
{
	Params p;
	auto *s = g_settings;
	p.enabled = s->getBool("fx_enable");
	p.hdr_composite = s->getBool("fx_hdr_composite");
	p.tonemapper = s->getS32("fx_tonemapper");
	p.exposure = s->getFloat("fx_exposure");
	p.grade_saturation = s->getFloat("fx_grade_saturation");
	p.grade_contrast = s->getFloat("fx_grade_contrast");
	p.bloom_threshold = s->getFloat("fx_bloom_threshold");
	p.bloom_knee = s->getFloat("fx_bloom_knee");
	p.bloom_intensity = s->getFloat("fx_bloom_intensity");
	p.emissive_strength = s->getFloat("fx_emissive_strength");
	p.auto_emissive_threshold = s->getFloat("fx_auto_emissive_threshold");
	p.colored_light = s->getBool("fx_colored_light");
	p.light_saturation = s->getFloat("fx_light_saturation");
	p.light_gather_radius = s->getFloat("fx_light_gather_radius");
	p.flicker = s->getFloat("fx_flicker");
	p.held_light = s->getBool("fx_held_light");
	p.debug_view = s->getS32("fx_debug_view");
	p.hot_reload = s->getBool("fx_shader_hot_reload");
	s_params = p;
}

static void onSettingChanged(const std::string &, void *)
{
	readParams();
}

const Params &params()
{
	return s_params;
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

/*
	Material companions
*/

// A texture atom: a plain file name, without escapes or modifiers.
static const std::regex s_atom_re(R"(([A-Za-z0-9_.\-]+)\.png)");

std::string companionName(ITextureSource *tsrc, const std::string &image,
		bool auto_emissive, core::dimension2du size)
{
	// Split off a trailing mesh filter if present, it is re-applied at the end.
	std::string base = image;
	std::string suffix;
	const std::string &filter = ITextureSource::FILTER_FOR_MESH;
	if (!filter.empty() && str_ends_with(base, filter)) {
		base.resize(base.size() - filter.size());
		suffix = filter;
	}

	bool any_authored = false;
	std::string out;
	out.reserve(base.size() * 2);
	auto begin = std::sregex_iterator(base.begin(), base.end(), s_atom_re);
	size_t last = 0;
	for (auto it = begin; it != std::sregex_iterator(); ++it) {
		const auto &m = *it;
		out.append(base, last, m.position() - last);
		const std::string stem = m[1].str();
		const std::string authored = stem + "_e.png";
		// Atoms inside [combine etc. are escaped and would not match the
		// regex boundaries cleanly, so only rewrite atoms at top level.
		bool top_level = m.position() == 0 || base[m.position() - 1] == '^' ||
				base[m.position() - 1] == '(';
		if (top_level && tsrc->isKnownSourceImage(authored)) {
			out += authored;
			any_authored = true;
		} else if (top_level) {
			// keep coverage (alpha), drop color
			out += "(" + m.str() + "^[multiply:#000000)";
		} else {
			out += m.str();
		}
		last = m.position() + m.length();
	}
	out.append(base, last, std::string::npos);

	std::string result;
	if (any_authored) {
		result = out;
	} else if (auto_emissive) {
		std::ostringstream os;
		os << "(" << base << ")^[fx_autoemissive:" << s_params.auto_emissive_threshold;
		result = os.str();
	} else {
		// Nothing glows: a black layer with the base's alpha.
		result = "(" + base + ")^[multiply:#000000";
	}

	if (size.Width && size.Height) {
		auto csize = tsrc->getTextureDimensions(result);
		if (csize != size) {
			std::ostringstream os;
			os << "(" << result << ")^[resize:" << size.Width << "x" << size.Height;
			result = os.str();
		}
	}
	verbosestream << "fx: companion of \"" << image << "\" = \"" << result + suffix << "\"" << std::endl;
	return result + suffix;
}

void applyAutoEmissive(video::IImage *image, float threshold)
{
	auto dim = image->getDimension();
	const float t0 = std::clamp(threshold, 0.f, 0.99f);
	for (u32 y = 0; y < dim.Height; y++)
	for (u32 x = 0; x < dim.Width; x++) {
		video::SColor c = image->getPixel(x, y);
		float r = c.getRed() / 255.f, g = c.getGreen() / 255.f, b = c.getBlue() / 255.f;
		// Use max channel rather than luma: saturated flames (orange, red)
		// have low luma but are exactly the pixels that should glow.
		float v = std::max({r, g, b});
		float k = std::clamp((v - t0) / (1.f - t0), 0.f, 1.f);
		k = k * k * (3.f - 2.f * k);
		c.setRed(core::round32(r * k * 255.f));
		c.setGreen(core::round32(g * k * 255.f));
		c.setBlue(core::round32(b * k * 255.f));
		image->setPixel(x, y, c);
	}
}

/*
	Light colors
*/

static std::mutex s_light_table_mutex;
static bool s_light_table_loaded = false;
static std::unordered_map<std::string, LightStyle> s_light_table;

// fx_light_colors.txt, looked up in the texture pack paths:
//   # comment
//   mcl_torches:torch      #ffb060  flicker=1.0
//   mcl_nether:glowstone   #ffe0a0
static void loadLightTableFile(const std::string &path)
{
	std::ifstream is(path);
	std::string line;
	size_t n = 0;
	while (std::getline(is, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		auto parts = str_split(line, ' ');
		parts.erase(std::remove_if(parts.begin(), parts.end(),
				[] (const std::string &p) { return trim(p).empty(); }), parts.end());
		if (parts.size() < 2)
			continue;
		LightStyle st;
		if (!parseColorString(parts[1], st.color, true)) {
			warningstream << "fx: bad color in " << path << ": " << line << std::endl;
			continue;
		}
		for (size_t i = 2; i < parts.size(); i++) {
			if (str_starts_with(parts[i], "flicker="))
				st.flicker = std::clamp(mystof(parts[i].substr(8)), 0.f, 4.f);
		}
		// first texture dir wins, like texture lookups
		if (s_light_table.emplace(parts[0], st).second)
			n++;
	}
	infostream << "fx: loaded " << n << " light colors from " << path << std::endl;
}

// fx_light_colors.txt in any texture pack dir (same lookup as override.txt):
//   # comment
//   mcl_torches:torch      #ffb060  flicker=1.0
//   mcl_nether:glowstone   #ffe0a0
static void loadLightTable()
{
	s_light_table.clear();
	bool found = false;
	for (const auto &dir : getTextureDirs()) {
		const std::string path = dir + DIR_DELIM + "fx_light_colors.txt";
		if (!fs::PathExists(path))
			continue;
		loadLightTableFile(path);
		found = true;
	}
	if (!found)
		infostream << "fx: no fx_light_colors.txt in texture dirs, deriving all light colors" << std::endl;
}

void resetLightTable()
{
	std::lock_guard lock(s_light_table_mutex);
	s_light_table_loaded = false;
}

static video::SColor normalizeChroma(video::SColor c)
{
	// Scale so the brightest channel is 255: we only want the hue, the
	// intensity comes from the engine's occlusion-correct scalar light.
	u32 m = std::max({c.getRed(), c.getGreen(), c.getBlue()});
	if (m == 0)
		return video::SColor(255, 255, 255, 255);
	return video::SColor(255, c.getRed() * 255 / m, c.getGreen() * 255 / m,
			c.getBlue() * 255 / m);
}

LightStyle resolveLightStyle(ITextureSource *tsrc, const ContentFeatures &f)
{
	{
		std::lock_guard lock(s_light_table_mutex);
		if (!s_light_table_loaded) {
			loadLightTable();
			s_light_table_loaded = true;
		}
		auto it = s_light_table.find(f.name);
		if (it != s_light_table.end()) {
			LightStyle st = it->second;
			st.color = normalizeChroma(st.color);
			return st;
		}
	}

	LightStyle st;
	// Heuristic flicker: fire-like nodes flicker, solid glow blocks don't.
	const std::string &n = f.name;
	if (n.find("torch") != std::string::npos || n.find("fire") != std::string::npos ||
			n.find("flame") != std::string::npos || n.find("candle") != std::string::npos ||
			n.find("campfire") != std::string::npos)
		st.flicker = 1.f;
	else if (n.find("lava") != std::string::npos)
		st.flicker = 0.35f;

	// Auto-derive from the first non-empty tile: average of the glowing pixels.
	for (const auto &tile : f.tiledef) {
		if (tile.name.empty())
			continue;
		std::ostringstream os;
		os << "(" << tile.name << ")^[fx_autoemissive:" << s_params.auto_emissive_threshold;
		video::SColor avg = tsrc->getTextureAverageColor(os.str());
		if (avg.getRed() + avg.getGreen() + avg.getBlue() > 0)
			st.color = normalizeChroma(avg);
		break;
	}
	infostream << "fx: light " << f.name << " color=" << st.color.getRed() << ","
		<< st.color.getGreen() << "," << st.color.getBlue() << " flicker=" << st.flicker << std::endl;
	return st;
}

/*
	Block light lists
*/

void collectBlockLights(const MeshMakeData *data, std::vector<Light> &out)
{
	out.clear();
	const NodeDefManager *ndef = data->m_nodedef;
	const v3s16 base = data->m_blockpos * MAP_BLOCKSIZE;
	const s16 n = data->m_side_length;

	// Accumulate per (cluster cell, color). Meshes are at most a few thousand
	// nodes, so a flat vector with linear search per cell stays cheap.
	struct Acc {
		v3s16 cell;
		u32 color;
		v3f sum;
		v3f lo, hi;
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
		if (f.light_source == 0 || !f.visuals)
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
			acc.push_back({cell, color, pf, pf, pf, 1, f.light_source,
					f.visuals->fx_light.flicker});
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

/*
	Per-frame light gathering
*/

static FrameLights s_frame;

const FrameLights &frameLights()
{
	return s_frame;
}

static float hashSeed(v3s16 p)
{
	u32 h = (u32)p.X * 73856093u ^ (u32)p.Y * 19349663u ^ (u32)p.Z * 83492791u;
	return (h % 10007u) / 10007.f;
}

void updateFrame(Client *client, Camera *camera, float dtime)
{
	const Params &prm = s_params;
	s_frame.count = 0;
	if (!prm.enabled || !prm.colored_light)
		return;

	ClientMap &map = client->getEnv().getClientMap();
	const v3f cam_pos = camera->getPosition();             // BS units, world
	const v3s16 cam_offset = camera->getOffset();          // nodes
	const v3f offset_bs = intToFloat(cam_offset, BS);
	const v3s16 cam_node = floatToInt(cam_pos, BS);
	const float radius_nodes = std::max(16.f, prm.light_gather_radius);
	const s16 rb = (s16)std::ceil(radius_nodes / MAP_BLOCKSIZE);
	const v3s16 cam_block = getNodeBlockPos(cam_node);

	struct Candidate { float d2; const Light *l; };
	static std::vector<Candidate> cands;
	cands.clear();

	// Meshes may span a mesh-grid cell of several blocks, so dedupe them.
	static std::vector<const MapBlockMesh *> seen;
	seen.clear();

	v3s16 bp;
	for (bp.Z = cam_block.Z - rb; bp.Z <= cam_block.Z + rb; bp.Z++)
	for (bp.Y = cam_block.Y - rb; bp.Y <= cam_block.Y + rb; bp.Y++)
	for (bp.X = cam_block.X - rb; bp.X <= cam_block.X + rb; bp.X++) {
		MapBlock *block = map.getBlockNoCreateNoEx(bp);
		if (!block || !block->mesh)
			continue;
		const MapBlockMesh *mesh = block->mesh;
		if (std::find(seen.begin(), seen.end(), mesh) != seen.end())
			continue;
		seen.push_back(mesh);
		for (const Light &l : mesh->getFxLights()) {
			v3f lp = l.pos * BS;
			float d2 = (lp - cam_pos).getLengthSQ();
			if (d2 > radius_nodes * radius_nodes * BS * BS)
				continue;
			cands.push_back({d2, &l});
		}
	}

	const int max_world = MAX_LIGHTS - 1; // keep one slot for the held light
	if ((int)cands.size() > max_world) {
		std::nth_element(cands.begin(), cands.begin() + max_world, cands.end(),
				[] (const Candidate &a, const Candidate &b) { return a.d2 < b.d2; });
		cands.resize(max_world);
	}

	auto push = [&] (v3f pos_bs, float radius_bs, video::SColor c, float flicker, float seed,
			float strength) {
		int i = s_frame.count++;
		s_frame.pos_radius[i * 4 + 0] = pos_bs.X - offset_bs.X;
		s_frame.pos_radius[i * 4 + 1] = pos_bs.Y - offset_bs.Y;
		s_frame.pos_radius[i * 4 + 2] = pos_bs.Z - offset_bs.Z;
		s_frame.pos_radius[i * 4 + 3] = radius_bs;
		// Strength rides on the color magnitude: the shader normalizes hue
		// luminance afterwards, so this only weights the hue mix.
		s_frame.color_seed[i * 4 + 0] = c.getRed() / 255.f * strength;
		s_frame.color_seed[i * 4 + 1] = c.getGreen() / 255.f * strength;
		s_frame.color_seed[i * 4 + 2] = c.getBlue() / 255.f * strength;
		// integer part: flicker amplitude in 1/100ths, fraction: seed
		s_frame.color_seed[i * 4 + 3] =
				std::floor(std::clamp(flicker * prm.flicker, 0.f, 0.99f) * 100.f) + seed * 0.99f;
	};

	for (const auto &c : cands) {
		const Light &l = *c.l;
		// light_source 14 reaches ~14 nodes; add a little so the falloff
		// tail lands where the scalar light reaches zero.
		// A cluster of N emitters weighs like 1 + log2(N)/2 lights: a lava
		// lake should tint the cave, not drown the torch next to you.
		const v3s16 seed_pos(l.pos.X, l.pos.Y, l.pos.Z);
		push(l.pos * BS, (l.level + 1.5f + l.extent) * BS, l.color, l.flicker,
				hashSeed(seed_pos), 1.f + 0.5f * std::log2((float)l.count));
	}

	// Held light: a torch in hand lights the camera's surroundings.
	if (prm.held_light) {
		LocalPlayer *player = client->getEnv().getLocalPlayer();
		ItemStack selected, hand;
		const ItemStack &item = player->getWieldedItem(&selected, &hand);
		if (!item.empty()) {
			const NodeDefManager *ndef = client->ndef();
			content_t id;
			if (ndef->getId(item.name, id)) {
				const ContentFeatures &f = ndef->get(id);
				if (f.light_source > 0 && f.visuals) {
					v3f pos = cam_pos + camera->getDirection() * (0.6f * BS);
					push(pos, (f.light_source + 1.5f) * BS * 0.85f, f.visuals->fx_light.color,
							std::max(f.visuals->fx_light.flicker, 0.6f), 0.5f, 1.f);
				}
			}
		}
	}
	static float log_timer = 0.f;
	log_timer += dtime;
	if (log_timer > 5.f) {
		log_timer = 0.f;
		verbosestream << "fx: frame lights " << s_frame.count << std::endl;
		for (int i = 0; i < s_frame.count && i < 24; i++)
			verbosestream << "  fx light " << i << " pos=" << s_frame.pos_radius[i*4] << ","
				<< s_frame.pos_radius[i*4+1] << "," << s_frame.pos_radius[i*4+2]
				<< " r=" << s_frame.pos_radius[i*4+3] << " col=" << s_frame.color_seed[i*4]
				<< "," << s_frame.color_seed[i*4+1] << "," << s_frame.color_seed[i*4+2] << std::endl;
	}
}

/*
	Shader plumbing
*/

class FxConstantSetter : public IShaderConstantSetter
{
public:
	void onGenerate(const std::string &name, ShaderConstants &constants) override
	{
		const Params &p = s_params;
		if (!p.enabled)
			return;
		constants["FX_ENABLE"] = 1;
		constants["FX_LIGHT_MAX"] = MAX_LIGHTS;
		if (p.colored_light)
			constants["FX_COLORED_LIGHT"] = 1;
		if (p.hdr_composite)
			constants["FX_HDR_COMPOSITE"] = 1;
		constants["FX_TONEMAPPER"] = p.tonemapper;
		constants["FX_DEBUG_VIEW"] = p.debug_view;
	}
};

class FxUniformSetter : public IShaderUniformSetter
{
	CachedVertexShaderSetting<int> m_light_count{"fxLightCount"};
	CachedVertexShaderSetting<float, MAX_LIGHTS * 4, false> m_light_pos{"fxLightPosRadius"};
	CachedVertexShaderSetting<float, MAX_LIGHTS * 4, false> m_light_color{"fxLightColorSeed"};
	CachedVertexShaderSetting<float> m_light_saturation{"fxLightSaturation"};
	CachedPixelShaderSetting<float> m_emissive_strength{"fxEmissiveStrength"};
	CachedPixelShaderSetting<float> m_bloom_threshold{"fxBloomThreshold"};
	CachedPixelShaderSetting<float> m_bloom_knee{"fxBloomKnee"};
	CachedPixelShaderSetting<float> m_bloom_intensity{"fxBloomIntensity"};
	CachedPixelShaderSetting<float> m_exposure{"fxExposure"};
	CachedPixelShaderSetting<float> m_grade_saturation{"fxGradeSaturation"};
	CachedPixelShaderSetting<float> m_grade_contrast{"fxGradeContrast"};
	bool m_is_node;

public:
	FxUniformSetter(bool is_node) : m_is_node(is_node) {}

	void onSetUniforms(video::IMaterialRendererServices *services) override
	{
		const Params &p = s_params;
		if (m_is_node) {
			const FrameLights &fl = s_frame;
			int count = fl.count;
			m_light_count.set(&count, services);
			m_light_pos.set(fl.pos_radius, services);
			m_light_color.set(fl.color_seed, services);
			m_light_saturation.set(&p.light_saturation, services);
			m_emissive_strength.set(&p.emissive_strength, services);
		} else {
			m_bloom_threshold.set(&p.bloom_threshold, services);
			m_bloom_knee.set(&p.bloom_knee, services);
			m_bloom_intensity.set(&p.bloom_intensity, services);
			m_exposure.set(&p.exposure, services);
			m_grade_saturation.set(&p.grade_saturation, services);
			m_grade_contrast.set(&p.grade_contrast, services);
		}
	}
};

class FxUniformSetterFactory : public IShaderUniformSetterFactory
{
public:
	IShaderUniformSetter *create(const std::string &name) override
	{
		if (name == "nodes_shader")
			return new FxUniformSetter(true);
		if (name == "extract_bloom" || name == "second_stage")
			return new FxUniformSetter(false);
		return nullptr;
	}
};

std::unique_ptr<IShaderConstantSetter> createConstantSetter()
{
	return std::make_unique<FxConstantSetter>();
}

std::unique_ptr<IShaderUniformSetterFactory> createUniformSetterFactory()
{
	return std::make_unique<FxUniformSetterFactory>();
}

/*
	Shader hot reload
*/

static std::filesystem::file_time_type latestShaderMtime()
{
	namespace sfs = std::filesystem;
	std::vector<std::string> roots;
	std::string sp = g_settings->get("shader_path");
	if (!sp.empty())
		roots.push_back(sp);
	roots.push_back(porting::path_share + DIR_DELIM "client" DIR_DELIM "shaders");

	// not latest{}: libstdc++'s file_clock epoch is in 2174, newer than every real file
	sfs::file_time_type latest = sfs::file_time_type::min();
	std::error_code ec;
	for (const auto &root : roots) {
		if (!sfs::is_directory(root, ec))
			continue;
		for (auto it = sfs::recursive_directory_iterator(root, ec);
				!ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
			if (!it->is_regular_file(ec) || it->path().extension() != ".glsl")
				continue;
			auto t = it->last_write_time(ec);
			if (!ec && t > latest)
				latest = t;
		}
	}
	return latest;
}

void pollShaderHotReload(IWritableShaderSource *shsrc, float dtime)
{
	static float timer = 0.f;
	static std::filesystem::file_time_type last{};
	static bool primed = false;
	if (!s_params.hot_reload) {
		primed = false;
		return;
	}
	timer += dtime;
	if (timer < 0.5f)
		return;
	timer = 0.f;

	auto now = latestShaderMtime();
	if (!primed) {
		last = now;
		primed = true;
		infostream << "fx: shader hot reload armed, watching "
			<< (g_settings->get("shader_path").empty() ? std::string("built-in shaders")
				: g_settings->get("shader_path")) << std::endl;
		return;
	}
	if (now != last) {
		last = now;
		infostream << "fx: shader change detected, hot reloading" << std::endl;
		shsrc->hotReloadShaders();
	}
}

} // namespace fx
