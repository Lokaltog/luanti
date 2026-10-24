// Luanti "fx" fork: mood renderer, milestone 1 (light & glow)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "fx.h"
#include "light_volume.h"

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
#include <IRenderTarget.h>
#include <IVideoDriver.h>
#include <ICameraSceneNode.h>
#include <IMaterialRendererServices.h>
#include <algorithm>
#include <atomic>
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

// s_base: straight from the settings. s_params: effective values, i.e. the
// base plus the current mood's situational blend (see updateMood()).
static Params s_base;
static Params s_params;

static const char *const SETTING_NAMES[] = {
	"fx_enable", "fx_hdr_composite", "fx_tonemapper", "fx_exposure",
	"fx_grade_saturation", "fx_grade_contrast",
	"fx_bloom_threshold", "fx_bloom_knee", "fx_bloom_intensity",
	"fx_emissive_strength", "fx_auto_emissive_threshold",
	"fx_colored_light", "fx_light_saturation", "fx_light_gather_radius",
	"fx_flicker", "fx_held_light", "fx_debug_view", "fx_shader_hot_reload",
	"fx_surfaces", "fx_refraction", "fx_ssr", "fx_reflection",
	"fx_water_absorption", "fx_caustics", "fx_underwater_shafts",
	"fx_taa", "fx_taa_blend", "fx_sharpen", "fx_ao", "fx_ao_strength", "fx_ao_radius",
	"fx_sky_tint", "fx_ambient_floor", "fx_rim_light", "fx_light_volume",
	"fx_materials", "fx_normal_strength", "fx_parallax_depth", "fx_specular", "fx_glints",
	"fx_particles", "fx_heat_haze", "fx_contact_shadows",
};

// Float settings that moods and the tuner may change, with slider ranges.
struct FloatParam { const char *setting; const char *label; float min, max; float Params::*field; };
static const FloatParam FLOAT_PARAMS[] = {
	{"fx_exposure", "Exposure", 0.2f, 4.f, &Params::exposure},
	{"fx_grade_saturation", "Saturation", 0.f, 2.f, &Params::grade_saturation},
	{"fx_grade_contrast", "Contrast", 0.6f, 1.8f, &Params::grade_contrast},
	{"fx_bloom_intensity", "Bloom intensity", 0.f, 2.f, &Params::bloom_intensity},
	{"fx_bloom_threshold", "Bloom threshold", 0.2f, 4.f, &Params::bloom_threshold},
	{"fx_bloom_knee", "Bloom knee", 0.f, 1.f, &Params::bloom_knee},
	{"fx_emissive_strength", "Emissive strength", 0.f, 12.f, &Params::emissive_strength},
	{"fx_light_saturation", "Light colour", 0.f, 1.5f, &Params::light_saturation},
	{"fx_flicker", "Flicker", 0.f, 0.4f, &Params::flicker},
	{"fx_sky_tint", "Sky light tint", 0.f, 1.5f, &Params::sky_tint},
	{"fx_ambient_floor", "Ambient floor", 0.f, 0.15f, &Params::ambient_floor},
	{"fx_ao_strength", "Ambient occlusion", 0.f, 2.f, &Params::ao_strength},
	{"fx_rim_light", "Rim light (mobs)", 0.f, 1.f, &Params::rim_light},
	{"fx_refraction", "Refraction", 0.f, 3.f, &Params::refraction},
	{"fx_reflection", "Reflection", 0.f, 2.f, &Params::reflection},
	{"fx_water_absorption", "Water absorption", 0.f, 1.f, &Params::water_absorption},
	{"fx_caustics", "Caustics", 0.f, 2.f, &Params::caustics},
	{"fx_underwater_shafts", "Underwater shafts", 0.f, 2.f, &Params::underwater_shafts},
	{"fx_sharpen", "Sharpen", 0.f, 1.f, &Params::sharpen},
	{"fx_taa_blend", "TAA history", 0.5f, 0.97f, &Params::taa_blend},
	{"fx_normal_strength", "Normal maps", 0.f, 2.f, &Params::normal_strength},
	{"fx_parallax_depth", "Parallax depth", 0.f, 0.15f, &Params::parallax_depth},
	{"fx_specular", "Specular", 0.f, 2.f, &Params::specular},
	{"fx_glints", "Glints", 0.f, 2.f, &Params::glints},
	{"fx_heat_haze", "Heat haze", 0.f, 2.f, &Params::heat_haze},
	{"fx_contact_shadows", "Contact shadows", 0.f, 1.f, &Params::contact_shadows},
};

const std::vector<TunableParam> &tunableParams()
{
	static std::vector<TunableParam> v = [] {
		std::vector<TunableParam> r;
		for (const auto &f : FLOAT_PARAMS)
			r.push_back({f.setting, f.label, f.min, f.max});
		return r;
	}();
	return v;
}

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
	p.surfaces = s->getBool("fx_surfaces");
	p.refraction = s->getFloat("fx_refraction");
	p.ssr = s->getBool("fx_ssr");
	p.reflection = s->getFloat("fx_reflection");
	p.water_absorption = s->getFloat("fx_water_absorption");
	p.caustics = s->getFloat("fx_caustics");
	p.underwater_shafts = s->getFloat("fx_underwater_shafts");
	p.taa = s->getBool("fx_taa");
	p.taa_blend = s->getFloat("fx_taa_blend");
	p.sharpen = s->getFloat("fx_sharpen");
	p.ao = s->getBool("fx_ao");
	p.ao_strength = s->getFloat("fx_ao_strength");
	p.ao_radius = s->getFloat("fx_ao_radius");
	p.sky_tint = s->getFloat("fx_sky_tint");
	p.ambient_floor = s->getFloat("fx_ambient_floor");
	p.rim_light = s->getFloat("fx_rim_light");
	p.light_volume = s->getBool("fx_light_volume");
	p.materials = s->getBool("fx_materials");
	p.normal_strength = s->getFloat("fx_normal_strength");
	p.parallax_depth = s->getFloat("fx_parallax_depth");
	p.specular = s->getFloat("fx_specular");
	p.glints = s->getFloat("fx_glints");
	p.particles = s->getBool("fx_particles");
	p.heat_haze = s->getFloat("fx_heat_haze");
	p.contact_shadows = s->getFloat("fx_contact_shadows");
	s_base = p;
	s_params = p; // situational blend is re-applied next frame
}

/*
	Moods
*/

struct Mood {
	std::string name;
	std::vector<std::pair<std::string, float>> base;
	std::unordered_map<std::string, float> cave, night, underwater;
};
static Mood s_mood;
static float s_w_cave = 0.f, s_w_night = 0.f, s_w_underwater = 0.f;

static std::string moodPath(const std::string &name, bool user)
{
	return (user ? porting::path_user : porting::path_share + DIR_DELIM "client") +
		DIR_DELIM "fx_moods" DIR_DELIM + name + ".conf";
}

static bool loadMood(const std::string &name, Mood &out)
{
	std::string path = moodPath(name, true);
	if (!fs::PathExists(path))
		path = moodPath(name, false);
	std::ifstream is(path);
	if (!is.good())
		return false;
	out = Mood();
	out.name = name;
	std::string line;
	while (std::getline(is, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = trim(line.substr(0, eq));
		float value = mystof(trim(line.substr(eq + 1)));
		if (str_starts_with(key, "cave."))
			out.cave[key.substr(5)] = value;
		else if (str_starts_with(key, "night."))
			out.night[key.substr(6)] = value;
		else if (str_starts_with(key, "underwater."))
			out.underwater[key.substr(11)] = value;
		else if (str_starts_with(key, "fx_"))
			out.base.emplace_back(key, value);
	}
	infostream << "fx: loaded mood " << name << " from " << path << std::endl;
	return true;
}

static void applyMoodSetting()
{
	const std::string name = g_settings->get("fx_mood");
	if (name.empty() || name == s_mood.name)
		return;
	Mood m;
	if (!loadMood(name, m)) {
		warningstream << "fx: unknown mood \"" << name << "\"" << std::endl;
		return;
	}
	s_mood = m;
	// Selecting a mood writes its values into the settings, like a graphics
	// preset; tweaks made afterwards are the user's own.
	for (const auto &kv : m.base)
		g_settings->setFloat(kv.first, kv.second);
}

std::vector<std::string> listMoods()
{
	std::vector<std::string> out;
	for (bool user : {false, true}) {
		const std::string dir = (user ? porting::path_user : porting::path_share + DIR_DELIM "client")
			+ DIR_DELIM "fx_moods";
		for (const auto &e : fs::GetDirListing(dir)) {
			if (e.dir || !str_ends_with(e.name, ".conf"))
				continue;
			std::string n = e.name.substr(0, e.name.size() - 5);
			if (std::find(out.begin(), out.end(), n) == out.end())
				out.push_back(n);
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string saveMood(const std::string &raw_name)
{
	std::string name;
	for (char c : raw_name)
		if (std::isalnum((unsigned char)c) || c == '_' || c == '-')
			name += c;
	if (name.empty())
		return "";
	const std::string dir = porting::path_user + DIR_DELIM "fx_moods";
	fs::CreateAllDirs(dir);
	const std::string path = dir + DIR_DELIM + name + ".conf";
	std::ostringstream os;
	os << "# fx mood saved from the in-game tuner\n";
	for (const auto &f : FLOAT_PARAMS)
		os << f.setting << " = " << g_settings->getFloat(f.setting) << "\n";
	auto dump = [&] (const char *prefix, const std::unordered_map<std::string, float> &m) {
		for (const auto &kv : m)
			os << prefix << kv.first << " = " << kv.second << "\n";
	};
	dump("cave.", s_mood.cave);
	dump("night.", s_mood.night);
	dump("underwater.", s_mood.underwater);
	if (!fs::safeWriteToFile(path, os.str()))
		return "";
	s_mood.name = name;
	g_settings->set("fx_mood", name);
	return path;
}

// Blend the situational targets into s_params (floats only).
static void updateMood(float dtime, float cave_target, float night_target, float underwater_target)
{
	// ~1.5 s exponential smoothing, like an eye adapting
	const float k = 1.f - std::exp(-dtime / 1.5f);
	s_w_cave += (cave_target - s_w_cave) * k;
	s_w_night += (night_target - s_w_night) * k;
	s_w_underwater += (underwater_target - s_w_underwater) * k;

	for (const auto &f : FLOAT_PARAMS) {
		float v = s_base.*(f.field);
		auto blend = [&] (const std::unordered_map<std::string, float> &m, float w) {
			auto it = m.find(f.setting);
			if (it != m.end())
				v += (it->second - v) * w;
		};
		blend(s_mood.night, s_w_night);
		blend(s_mood.cave, s_w_cave);
		blend(s_mood.underwater, s_w_underwater);
		s_params.*(f.field) = v;
	}
}

void gradeSunlight(float rgb[3], float tod)
{
	const float amount = s_params.enabled ? s_params.sky_tint : 0.f;
	if (amount <= 0.f)
		return;
	// sun elevation, -1 (midnight) .. 1 (noon)
	const float e = std::sin((tod - 0.25f) * 2.f * core::PI);
	// golden hour: warm when the sun is low, strongest just above the horizon
	const float golden = std::clamp(1.f - std::fabs(e - 0.08f) / 0.25f, 0.f, 1.f);
	// night: moonlight is blue
	const float night = std::clamp(-e * 4.f, 0.f, 1.f);
	const float warm[3] = {1.f, 0.78f, 0.55f};
	const float moon[3] = {0.65f, 0.8f, 1.15f};
	for (int i = 0; i < 3; i++) {
		float tint = 1.f + (warm[i] - 1.f) * golden;
		tint += (moon[i] - 1.f) * night;
		rgb[i] *= 1.f + (tint - 1.f) * amount;
	}
}

// Settings callbacks run under a non-recursive mutex, so a mood (which sets
// more settings) is applied from the next frame, not from the callback.
static std::atomic<bool> s_mood_pending{false};
static std::atomic<bool> s_tuner_requested{false};

void requestTuner()
{
	s_tuner_requested = true;
}

bool consumeTunerRequest()
{
	return s_tuner_requested.exchange(false);
}

static void onSettingChanged(const std::string &name, void *)
{
	if (name == "fx_mood") {
		s_mood_pending = true;
		return;
	}
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
	g_settings->registerChangedCallback("fx_mood", &onSettingChanged, &s_params);
	// test hook: open the tuner right away (util/fx/smoketest.sh)
	if (g_settings->getBool("fx_open_tuner_on_start"))
		requestTuner();
	// load the selected mood's situational targets without re-applying its
	// base values (the user may have tuned them since)
	Mood m;
	if (loadMood(g_settings->get("fx_mood"), m))
		s_mood = m;
}

void deinit()
{
	destroyLightVolume();
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

bool materialLayers(ITextureSource *tsrc, const std::string &image,
		core::dimension2du size, std::string &normal, std::string &specular)
{
	// base atom: the first plain file name at the start of the string
	std::smatch m;
	std::string stem;
	if (std::regex_search(image, m, s_atom_re) && m.position() == 0)
		stem = m[1].str();
	std::ostringstream fill;
	auto make = [&] (const char *suffix, const char *neutral, std::string &out) {
		const std::string authored = stem + suffix;
		if (!stem.empty() && tsrc->isKnownSourceImage(authored)) {
			std::ostringstream os;
			os << "(" << authored << ")^[resize:" << size.Width << "x" << size.Height;
			out = os.str();
			return true;
		}
		std::ostringstream os;
		os << "[fill:" << size.Width << "x" << size.Height << ":" << neutral;
		out = os.str();
		return false;
	};
	// LabPBR neutrals: flat normal, no AO, top height / rough, dielectric, no emission
	const bool n = make("_n.png", "#8080ffff", normal);
	const bool s = make("_s.png", "#000000ff", specular);
	return n || s;
}

video::ITexture *neutralMaterialTexture(ITextureSource *tsrc)
{
	static video::ITexture *tex = nullptr;
	static ITextureSource *owner = nullptr;
	if (!tex || owner != tsrc) {
		tex = tsrc->addArrayTexture({"[fill:1x1:#8080ffff", "[fill:1x1:#000000ff"});
		owner = tsrc;
	}
	return tex;
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
	Glass and water surfaces
*/

static std::mutex s_surface_mutex;
static bool s_surface_table_loaded = false;
static std::unordered_map<std::string, Surface> s_surface_table;

static void loadSurfaceTable()
{
	s_surface_table.clear();
	for (const auto &dir : getTextureDirs()) {
		const std::string path = dir + DIR_DELIM + "fx_materials.txt";
		if (!fs::PathExists(path))
			continue;
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
			Surface s;
			if (parts[1] == "glass")
				s.kind = SurfaceKind::Glass;
			else if (parts[1] == "water")
				s.kind = SurfaceKind::Water;
			else if (parts[1] != "none") {
				warningstream << "fx: unknown surface \"" << parts[1] << "\" in " << path << std::endl;
				continue;
			}
			for (size_t i = 2; i < parts.size(); i++) {
				if (parts[i] == "pattern=fluted")
					s.pattern = SurfacePattern::Fluted;
				else if (parts[i] == "pattern=frosted")
					s.pattern = SurfacePattern::Frosted;
				else if (parts[i] == "pattern=rippled")
					s.pattern = SurfacePattern::Rippled;
			}
			if (s_surface_table.emplace(parts[0], s).second)
				n++;
		}
		infostream << "fx: loaded " << n << " surfaces from " << path << std::endl;
	}
}

Surface resolveSurface(const ContentFeatures &f)
{
	if (!s_params.enabled || !s_params.surfaces)
		return {};
	{
		std::lock_guard lock(s_surface_mutex);
		if (!s_surface_table_loaded) {
			loadSurfaceTable();
			s_surface_table_loaded = true;
		}
		auto it = s_surface_table.find(f.name);
		if (it != s_surface_table.end())
			return it->second;
	}
	Surface s;
	const bool liquid = f.drawtype == NDT_LIQUID || f.drawtype == NDT_FLOWINGLIQUID;
	if (liquid && f.light_source == 0 && f.alpha != ALPHAMODE_OPAQUE) {
		s.kind = SurfaceKind::Water;
	} else if (f.drawtype == NDT_GLASSLIKE || f.drawtype == NDT_GLASSLIKE_FRAMED ||
			f.drawtype == NDT_GLASSLIKE_FRAMED_OPTIONAL ||
			(f.name.find("glass") != std::string::npos && f.light_source == 0 &&
			 f.drawtype != NDT_NORMAL)) {
		s.kind = SurfaceKind::Glass;
	}
	if (s.kind == SurfaceKind::Glass) {
		if (f.name.find("fluted") != std::string::npos || f.name.find("reeded") != std::string::npos)
			s.pattern = SurfacePattern::Fluted;
		else if (f.name.find("frosted") != std::string::npos)
			s.pattern = SurfacePattern::Frosted;
		else if (f.name.find("ripple") != std::string::npos)
			s.pattern = SurfacePattern::Rippled;
	}
	return s;
}

bool handlesUnderwater(const ContentFeatures &f)
{
	static const bool post = g_settings->getBool("enable_post_processing");
	return post && s_params.enabled && s_params.surfaces && s_params.hdr_composite &&
			resolveSurface(f).kind == SurfaceKind::Water;
}

/*
	Scene copy (opaque color + depth, for the transparent pass)
*/

struct SceneCopy {
	video::IRenderTarget *rt = nullptr;
	video::ITexture *color = nullptr;
	video::ITexture *depth = nullptr;
	core::dimension2du size;
	video::ECOLOR_FORMAT color_format = video::ECF_UNKNOWN;
	video::ECOLOR_FORMAT depth_format = video::ECF_UNKNOWN;
	bool valid = false;
};
static SceneCopy s_copy;

// Camera state captured at the start of the transparent pass, while the
// driver still holds the 3D transforms. Post passes reuse it.
struct CameraState {
	core::matrix4 view_proj;     // scene space (relative to camera offset) -> clip
	core::matrix4 inv_view_proj;
	float near_far[2] = {1.f, 1000.f};
	float sun_dir[3] = {0.f, 1.f, 0.f}; // towards the sun, world space
	float sun_strength = 1.f;
	float underwater = 0.f;
	float time = 0.f;            // seconds, wraps every hour
	// TAA
	core::matrix4 view_proj_unjittered;
	core::matrix4 prev_view_proj;  // previous frame, unjittered, its own scene space
	v3s16 offset;                  // camera offset (nodes) this frame
	v3s16 prev_offset;
	float offset_delta[3] = {};    // (offset - prev_offset) * BS, scene units
	float jitter[2] = {};          // this frame, in NDC
	u32 frame = 0;
	u32 frames_since_reset = 0;
	core::dimension2du target_size;
	float eye[3] = {};             // camera position in scene space
	float proj_scale = 1.f;        // pixels per unit at view depth 1 (vertical)
};
static CameraState s_cam;

static float halton(u32 i, u32 base)
{
	float f = 1.f, r = 0.f;
	while (i > 0) {
		f /= base;
		r += f * (i % base);
		i /= base;
	}
	return r;
}

static core::matrix4 s_saved_projection;
static bool s_jitter_applied = false;

void applyJitter(scene::ICameraSceneNode *camera, core::dimension2du target_size)
{
	s_cam.jitter[0] = s_cam.jitter[1] = 0.f;
	if (!s_params.enabled || !s_params.taa || !camera || !target_size.Width)
		return;
	if (target_size != s_cam.target_size) {
		s_cam.target_size = target_size;
		s_cam.frames_since_reset = 0;
	}
	// 8-sample Halton(2,3), centred on the pixel, in NDC units
	const u32 i = (s_cam.frame % 8) + 1;
	s_cam.jitter[0] = (halton(i, 2) - 0.5f) * 2.f / target_size.Width;
	s_cam.jitter[1] = (halton(i, 3) - 0.5f) * 2.f / target_size.Height;
	s_saved_projection = camera->getProjectionMatrix();
	core::matrix4 p = s_saved_projection;
	// Irrlicht is left-handed with clip.w = z_view, so adding to the third
	// row's x/y shifts NDC by exactly that amount at every depth.
	p[8] += s_cam.jitter[0];
	p[9] += s_cam.jitter[1];
	camera->setProjectionMatrix(p, false);
	s_jitter_applied = true;
}

void removeJitter(scene::ICameraSceneNode *camera)
{
	if (!s_jitter_applied || !camera)
		return;
	camera->setProjectionMatrix(s_saved_projection, false);
	s_jitter_applied = false;
}

void captureScene(video::IVideoDriver *driver)
{
	s_copy.valid = false;
	if (!s_params.enabled)
		return;

	core::matrix4 proj = driver->getTransform(video::ETS_PROJECTION);
	const core::matrix4 &view = driver->getTransform(video::ETS_VIEW);
	core::matrix4 vp = proj;
	vp *= view;
	s_cam.view_proj = vp;
	vp.getInverse(s_cam.inv_view_proj);
	proj[8] -= s_cam.jitter[0];
	proj[9] -= s_cam.jitter[1];
	s_cam.proj_scale = proj[5] * 0.5f * driver->getCurrentRenderTargetSize().Height;
	s_cam.view_proj_unjittered = proj;
	s_cam.view_proj_unjittered *= view;

	if (!s_params.surfaces)
		return;

	video::IRenderTarget *src = driver->getCurrentRenderTarget();
	if (!src || src->getTexture().empty() || !src->getDepthStencil())
		return; // drawing straight to the back buffer: no post-processing
	video::ITexture *src_color = src->getTexture()[0];
	video::ITexture *src_depth = src->getDepthStencil();
	const auto size = src_color->getSize();
	const auto cf = src_color->getColorFormat();
	const auto df = src_depth->getColorFormat();

	if (!s_copy.rt || s_copy.size != size || s_copy.color_format != cf ||
			s_copy.depth_format != df) {
		if (s_copy.rt)
			driver->removeRenderTarget(s_copy.rt);
		if (s_copy.color)
			driver->removeTexture(s_copy.color);
		if (s_copy.depth)
			driver->removeTexture(s_copy.depth);
		s_copy.color = driver->addRenderTargetTexture(size, "fx_scene_color", cf);
		s_copy.depth = driver->addRenderTargetTexture(size, "fx_scene_depth", df);
		s_copy.rt = driver->addRenderTarget();
		if (!s_copy.color || !s_copy.depth || !s_copy.rt) {
			warningstream << "fx: could not create the scene copy target" << std::endl;
			s_copy = SceneCopy();
			return;
		}
		s_copy.rt->setTexture(s_copy.color, s_copy.depth);
		s_copy.size = size;
		s_copy.color_format = cf;
		s_copy.depth_format = df;
		infostream << "fx: scene copy " << size.Width << "x" << size.Height << std::endl;
	}
	// glBlitFramebuffer: resolves MSAA too, and restores the previous FBO
	driver->blitRenderTarget(src, s_copy.rt);
	s_copy.valid = true;
}

void bindSceneCopy(video::SMaterial &material)
{
	if (!s_copy.valid)
		return;
	auto &c = material.TextureLayers[TEXTURE_LAYER_SCENE_COLOR];
	c.Texture = s_copy.color;
	c.TextureWrapU = c.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	c.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	c.MagFilter = video::ETMAGF_LINEAR;
	auto &d = material.TextureLayers[TEXTURE_LAYER_SCENE_DEPTH];
	d.Texture = s_copy.depth;
	d.TextureWrapU = d.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	d.MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
	d.MagFilter = video::ETMAGF_NEAREST;
}

void bindLightVolume(video::SMaterial &material)
{
	if (!s_params.enabled || !s_params.light_volume)
		return;
	LightVolume *vol = lightVolume();
	if (!vol->valid())
		return;
	auto &l = material.TextureLayers[TEXTURE_LAYER_LIGHT_VOLUME];
	l.Texture = vol->texture();
	l.TextureWrapU = l.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	l.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	l.MagFilter = video::ETMAGF_LINEAR;
}

void unbindSceneCopy(video::SMaterial &material)
{
	material.TextureLayers[TEXTURE_LAYER_SCENE_COLOR].Texture = nullptr;
	material.TextureLayers[TEXTURE_LAYER_SCENE_DEPTH].Texture = nullptr;
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
	if (s_mood_pending.exchange(false))
		applyMoodSetting();

	const Params &prm = s_params;
	s_frame.count = 0;
	s_frame.held_index = -1;
	if (!prm.enabled)
		return;

	ClientMap &map = client->getEnv().getClientMap();
	const v3f cam_pos = camera->getPosition();             // BS units, world

	// Camera and sun state for glass, water and post passes
	{
		auto *node = camera->getCameraNode();
		s_cam.near_far[0] = node->getNearValue();
		s_cam.near_far[1] = node->getFarValue();
		// Luanti's sun rises in +X at 0.25 and peaks at 0.5
		const float tod = client->getEnv().getTimeOfDayF();
		const float a = (tod - 0.25f) * 2.f * core::PI;
		v3f sun(std::cos(a), std::sin(a), 0.15f);
		if (sun.Y < 0.f) // moon: opposite side, dimmer
			sun = -sun;
		sun.normalize();
		s_cam.sun_dir[0] = sun.X;
		s_cam.sun_dir[1] = sun.Y;
		s_cam.sun_dir[2] = sun.Z;
		s_cam.sun_strength = std::clamp(std::sin(a) * 3.f, 0.08f, 1.f);
		const MapNode n = map.getNode(floatToInt(cam_pos, BS));
		s_cam.underwater = resolveSurface(client->ndef()->get(n)).kind == SurfaceKind::Water ? 1.f : 0.f;

		// mood: cave = little sky light reaching the camera (works day and night,
		// the day bank holds sky light), night = day/night ratio
		const float sky = n.getLight(LIGHTBANK_DAY, client->ndef()->getLightingFlags(n));
		const float cave = std::clamp((11.f - sky) / 8.f, 0.f, 1.f);
		const float dnr = client->getEnv().getDayNightRatio() / 1000.f;
		const float night = std::clamp((0.6f - dnr) / 0.4f, 0.f, 1.f);
		updateMood(dtime, cave, night, s_cam.underwater);
		s_cam.time = std::fmod(s_cam.time + dtime, 3600.f);

		// TAA bookkeeping: last frame's unjittered matrix becomes "previous"
		s_cam.prev_view_proj = s_cam.view_proj_unjittered;
		s_cam.prev_offset = s_cam.offset;
		s_cam.offset = camera->getOffset();
		const v3f eye = cam_pos - intToFloat(s_cam.offset, BS);
		s_cam.eye[0] = eye.X;
		s_cam.eye[1] = eye.Y;
		s_cam.eye[2] = eye.Z;
		const v3s16 d = s_cam.offset - s_cam.prev_offset;
		s_cam.offset_delta[0] = d.X * BS;
		s_cam.offset_delta[1] = d.Y * BS;
		s_cam.offset_delta[2] = d.Z * BS;
		if (d != v3s16(0, 0, 0) || s_cam.frame == 0)
			s_cam.frames_since_reset = 0;
		s_cam.frame++;
		s_cam.frames_since_reset++;
	}

	if (prm.light_volume && prm.colored_light)
		lightVolume()->update(client, floatToInt(cam_pos, BS), dtime);

	if (!prm.colored_light)
		return;
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
					s_frame.held_index = s_frame.count;
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
		// the pipeline skips TAA with MSAA/SSAA; keep the define in sync
		const std::string aa = g_settings->get("antialiasing");
		if (p.light_volume && p.colored_light)
			constants["FX_LIGHT_VOLUME"] = 1;
		if (p.materials)
			constants["FX_MATERIALS"] = 1;
		if (p.taa && aa != "fsaa" && aa != "ssaa")
			constants["FX_TAA"] = 1;
		if (p.ao)
			constants["FX_AO"] = 1;
		if (p.surfaces) {
			constants["FX_SURFACES"] = 1;
			if (p.ssr)
				constants["FX_SSR"] = 1;
		}
	}
};

class FxUniformSetter : public IShaderUniformSetter
{
	CachedVertexShaderSetting<int> m_light_count{"fxLightCount"};
	CachedVertexShaderSetting<float, MAX_LIGHTS * 4, false> m_light_pos{"fxLightPosRadius"};
	CachedVertexShaderSetting<float, MAX_LIGHTS * 4, false> m_light_color{"fxLightColorSeed"};
	CachedVertexShaderSetting<float> m_light_saturation{"fxLightSaturation"};
	CachedVertexShaderSetting<float> m_ambient_floor{"fxAmbientFloor"};
	CachedVertexShaderSetting<float, 3> m_volume_origin{"fxVolumeOrigin"};
	CachedVertexShaderSetting<float> m_volume_valid{"fxVolumeValid"};
	CachedVertexShaderSetting<int> m_held_index{"fxHeldLightIndex"};
	CachedPixelShaderSetting<float> m_rim_light{"fxRimLight"};
	CachedPixelShaderSetting<float> m_normal_strength{"fxNormalStrength"};
	CachedPixelShaderSetting<float> m_parallax_depth{"fxParallaxDepth"};
	CachedPixelShaderSetting<float> m_specular{"fxSpecular"};
	CachedPixelShaderSetting<float> m_glints{"fxGlints"};
	CachedPixelShaderSetting<float> m_heat_haze{"fxHeatHaze"};
	CachedPixelShaderSetting<float> m_contact_shadows{"fxContactShadows"};
	CachedPixelShaderSetting<float> m_emissive_strength{"fxEmissiveStrength"};
	CachedPixelShaderSetting<float> m_bloom_threshold{"fxBloomThreshold"};
	CachedPixelShaderSetting<float> m_bloom_knee{"fxBloomKnee"};
	CachedPixelShaderSetting<float> m_bloom_intensity{"fxBloomIntensity"};
	CachedPixelShaderSetting<float> m_exposure{"fxExposure"};
	CachedPixelShaderSetting<float> m_grade_saturation{"fxGradeSaturation"};
	CachedPixelShaderSetting<float> m_grade_contrast{"fxGradeContrast"};
	// M3
	CachedPixelShaderSetting<float, 16> m_view_proj{"fxViewProj"};
	CachedPixelShaderSetting<float, 16> m_inv_view_proj{"fxInvViewProj"};
	CachedPixelShaderSetting<float, 2> m_near_far{"fxNearFar"};
	CachedPixelShaderSetting<float, 3> m_sun_dir{"fxSunDir"};
	CachedPixelShaderSetting<float> m_sun_strength{"fxSunStrength"};
	CachedPixelShaderSetting<float> m_time{"fxTime"};
	CachedPixelShaderSetting<float> m_refraction{"fxRefraction"};
	CachedPixelShaderSetting<float> m_reflection{"fxReflection"};
	CachedPixelShaderSetting<float> m_water_absorption{"fxWaterAbsorption"};
	CachedPixelShaderSetting<float> m_caustics{"fxCaustics"};
	CachedPixelShaderSetting<float> m_underwater{"fxUnderwater"};
	CachedPixelShaderSetting<float> m_shafts{"fxUnderwaterShafts"};
	CachedPixelShaderSetting<float> m_scene_valid{"fxSceneValid"};
	CachedPixelShaderSetting<float, 16> m_prev_view_proj{"fxPrevViewProj"};
	CachedPixelShaderSetting<float, 3> m_offset_delta{"fxOffsetDelta"};
	CachedPixelShaderSetting<float, 2> m_jitter{"fxJitter"};
	CachedPixelShaderSetting<float> m_taa_blend{"fxTaaBlend"};
	CachedPixelShaderSetting<float> m_taa_reset{"fxTaaReset"};
	CachedPixelShaderSetting<float> m_sharpen{"fxSharpen"};
	CachedPixelShaderSetting<float> m_ao_strength{"fxAoStrength"};
	CachedPixelShaderSetting<float, 3> m_eye{"fxEyePos"};
	CachedPixelShaderSetting<float> m_proj_scale{"fxProjScale"};
	CachedPixelShaderSetting<float> m_frame{"fxFrame"};
	CachedPixelShaderSetting<float> m_ao_radius{"fxAoRadius"};
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
			m_ambient_floor.set(&p.ambient_floor, services);
			{
				LightVolume *vol = p.light_volume ? lightVolume() : nullptr;
				float valid = vol && vol->valid() ? 1.f : 0.f;
				// scene-space centre of voxel (0,0,0)
				const v3s16 o = vol ? vol->origin() : v3s16();
				float origin[3] = {
					(float)(o.X - s_cam.offset.X) * BS,
					(float)(o.Y - s_cam.offset.Y) * BS,
					(float)(o.Z - s_cam.offset.Z) * BS};
				m_volume_origin.set(origin, services);
				m_volume_valid.set(&valid, services);
				int held = fl.held_index;
				m_held_index.set(&held, services);
			}
			m_rim_light.set(&p.rim_light, services);
			m_normal_strength.set(&p.normal_strength, services);
			m_parallax_depth.set(&p.parallax_depth, services);
			m_specular.set(&p.specular, services);
			m_glints.set(&p.glints, services);
			m_emissive_strength.set(&p.emissive_strength, services);
			float valid = s_copy.valid ? 1.f : 0.f;
			m_scene_valid.set(&valid, services);
		} else {
			m_inv_view_proj.set(s_cam.inv_view_proj.pointer(), services);
			m_underwater.set(&s_cam.underwater, services);
			m_shafts.set(&p.underwater_shafts, services);
			m_heat_haze.set(&p.heat_haze, services);
			m_contact_shadows.set(&p.contact_shadows, services);
			m_prev_view_proj.set(s_cam.prev_view_proj.pointer(), services);
			m_offset_delta.set(s_cam.offset_delta, services);
			m_jitter.set(s_cam.jitter, services);
			m_taa_blend.set(&p.taa_blend, services);
			float reset = s_cam.frames_since_reset < 2 ? 1.f : 0.f;
			m_taa_reset.set(&reset, services);
			float sharpen = p.taa ? p.sharpen : 0.f;
			m_sharpen.set(&sharpen, services);
			m_ao_strength.set(&p.ao_strength, services);
			m_eye.set(s_cam.eye, services);
			m_proj_scale.set(&s_cam.proj_scale, services);
			float frame = (float)(s_cam.frame % 1024);
			m_frame.set(&frame, services);
			m_ao_radius.set(&p.ao_radius, services);
			m_bloom_threshold.set(&p.bloom_threshold, services);
			m_bloom_knee.set(&p.bloom_knee, services);
			m_bloom_intensity.set(&p.bloom_intensity, services);
			m_exposure.set(&p.exposure, services);
			m_grade_saturation.set(&p.grade_saturation, services);
			m_grade_contrast.set(&p.grade_contrast, services);
		}
		// shared by node surfaces and post passes
		m_view_proj.set(s_cam.view_proj.pointer(), services);
		m_near_far.set(s_cam.near_far, services);
		m_sun_dir.set(s_cam.sun_dir, services);
		m_sun_strength.set(&s_cam.sun_strength, services);
		m_time.set(&s_cam.time, services);
		m_refraction.set(&p.refraction, services);
		m_reflection.set(&p.reflection, services);
		m_water_absorption.set(&p.water_absorption, services);
		m_caustics.set(&p.caustics, services);
	}
};

class FxUniformSetterFactory : public IShaderUniformSetterFactory
{
public:
	IShaderUniformSetter *create(const std::string &name) override
	{
		if (name == "nodes_shader" || name == "object_shader")
			return new FxUniformSetter(true);
		if (name == "extract_bloom" || name == "second_stage" ||
				name == "fx_taa" || name == "fx_ao")
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
