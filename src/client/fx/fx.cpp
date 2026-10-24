// Luanti "fx" fork: lifecycle, settings and moods
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "renderer.h"
#include "fx_test.h"

#include "client/camera.h"
#include "client/client.h"
#include "client/shader.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/string.h"
#include <IVideoDriver.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fx
{

namespace
{

Renderer *s_renderer = nullptr;

const Caps s_off_caps;
const Params s_default_params;

void onSettingChanged(const std::string &name, void *data)
{
	// Settings callbacks run under the settings mutex, on whichever thread
	// set the value (the tuner, a mod in singleplayer). Only flag it here.
	auto *r = static_cast<Renderer *>(data);
	if (name == "fx_mood")
		r->mood_dirty = true;
	else
		r->settings_dirty = true;
}

std::string moodDir(bool user)
{
	return (user ? porting::path_user : porting::path_share + DIR_DELIM "client") +
		DIR_DELIM "fx_moods";
}

} // namespace

Renderer *renderer()
{
	return s_renderer;
}

/*
	Lifecycle
*/

Renderer::Renderer(video::IVideoDriver *drv, IWritableShaderSource *src) :
	driver(drv), shsrc(src)
{
	// a mood picked while no world ran (main menu settings) applies now
	const std::string want = g_settings->get("fx_mood");
	if (!want.empty() && want != g_settings->get("fx_mood_applied"))
		applyMood(want);
	else
		loadMood(want, mood);

	base = readParams(*g_settings);
	params = base;
	last_shader_params = base;
	caps = computeCaps(base, driver);

	for (const auto &i : paramInfo())
		g_settings->registerChangedCallback(i.setting, &onSettingChanged, this);
	g_settings->registerChangedCallback("fx_mood", &onSettingChanged, this);

	if (!caps.enabled)
		return;
	loadPackTables(pack);
	driver->setUniformBlockBinding("FxParams", UBO_PARAMS);
	driver->setUniformBlockBinding("FxFrame", UBO_FRAME);
	driver->setUniformBlockBinding("FxLights", UBO_LIGHTS);
	if (caps.light_volume)
		volume = std::make_unique<LightVolume>(driver);
	uploadParams();
	uploadFrame();
	if (g_settings->getBool("fx_open_tuner_on_start"))
		tuner_requested = true;
}

Renderer::~Renderer()
{
	g_settings->deregisterAllChangedCallbacks(this);
	volume.reset();
	if (copy.rt)
		driver->removeRenderTarget(copy.rt);
	if (copy.color)
		driver->removeTexture(copy.color);
	if (copy.depth)
		driver->removeTexture(copy.depth);
	// neutral_material belongs to the TextureSource, which frees it
}

void attach(video::IVideoDriver *driver, IWritableShaderSource *shsrc)
{
	delete s_renderer;
	s_renderer = new Renderer(driver, shsrc);
	registerShaderSetters(*s_renderer);
	test::onGameStart();
}

void detach()
{
	delete s_renderer;
	s_renderer = nullptr;
}

const Caps &caps()
{
	return s_renderer ? s_renderer->caps : s_off_caps;
}

const Params &params()
{
	return s_renderer ? s_renderer->params : s_default_params;
}

/*
	Settings
*/

void Renderer::onSettingsChanged()
{
	base = readParams(*g_settings);
	// RESTART settings stay as the caps froze them; SHADERS ones rebuild
	bool rebuild = false;
	std::string restart;
	for (const auto &i : paramInfo()) {
		const float now = getFloat(base, i), was = getFloat(last_shader_params, i);
		if (now == was)
			continue;
		if (i.apply == Apply::SHADERS)
			rebuild = true;
		else if (i.apply == Apply::RESTART)
			restart += (restart.empty() ? "" : ", ") + i.setting;
	}
	note = restart.empty() ? "" : "Applies when you rejoin: " + restart;
	if (!restart.empty())
		infostream << "fx: " << note << std::endl;
	if (rebuild && caps.enabled) {
		for (const auto &i : paramInfo())
			if (i.apply == Apply::SHADERS)
				setFloat(last_shader_params, i, getFloat(base, i));
		infostream << "fx: shader setting changed, rebuilding shaders" << std::endl;
		shsrc->hotReloadShaders();
	}
}

/*
	Moods
*/

const std::vector<std::string> &situations()
{
	static const std::vector<std::string> list = {"night", "cave", "underwater",
		"rain", "snow", "storm", "nether", "heat", "end", "portal"};
	return list;
}

static bool loadMoodFile(const std::string &name, Mood &out);

// A mood file, over the shared situational targets in _situations.conf
// (the mood's own targets win, key by key).
bool loadMood(const std::string &name, Mood &out)
{
	if (!loadMoodFile(name, out))
		return false;
	Mood common;
	if (loadMoodFile("_situations", common)) {
		for (auto &[sit, map] : common.targets)
			for (auto &[info, v] : map)
				out.targets[sit].emplace(info, v); // keeps the mood's own
	}
	return true;
}

static bool loadMoodFile(const std::string &name, Mood &out)
{
	out = Mood();
	out.name = name;
	if (name.empty())
		return false;
	std::string path = moodDir(true) + DIR_DELIM + name + ".conf";
	if (!fs::PathExists(path))
		path = moodDir(false) + DIR_DELIM + name + ".conf";
	std::ifstream is(path);
	if (!is.good()) {
		warningstream << "fx: unknown mood \"" << name << "\"" << std::endl;
		return false;
	}
	std::string line;
	int lineno = 0;
	while (std::getline(is, line)) {
		lineno++;
		std::string t(trim(line));
		if (t.empty() || t[0] == '#')
			continue;
		auto eq = t.find('=');
		if (eq == std::string::npos) {
			warningstream << path << ":" << lineno << ": expected key = value" << std::endl;
			continue;
		}
		std::string key(trim(t.substr(0, eq)));
		std::string val(trim(t.substr(eq + 1)));
		auto *target = &out.base;
		// "<situation>.fx_x": the situation is everything before the last dot
		if (const auto dot = key.rfind('.'); dot != std::string::npos) {
			const std::string sit = key.substr(0, dot);
			const auto &known = situations();
			if (!str_starts_with(sit, "biome.") &&
					std::find(known.begin(), known.end(), sit) == known.end()) {
				warningstream << path << ":" << lineno << ": unknown situation \"" << sit
					<< "\"" << std::endl;
				continue;
			}
			target = &out.targets[sit];
			key = key.substr(dot + 1);
		}
		const ParamInfo *info = findParam(key);
		if (!info || !info->tune || info->type != ParamType::Float) {
			warningstream << path << ":" << lineno << ": \"" << key
				<< "\" is not a tunable fx setting" << std::endl;
			continue;
		}
		char *end = nullptr;
		const float v = std::strtof(val.c_str(), &end);
		if (end == val.c_str() || *end) {
			warningstream << path << ":" << lineno << ": \"" << val << "\" is not a number" << std::endl;
			continue;
		}
		(*target)[info] = std::clamp(v, info->min, info->max);
	}
	infostream << "fx: loaded mood " << name << " from " << path << std::endl;
	return true;
}

// A mood is a complete preset: every tunable it does not name goes back to
// its default, so switching moods never depends on which came before.
void Renderer::applyMood(const std::string &name)
{
	Mood m;
	if (!loadMood(name, m))
		return;
	mood = m;
	for (const auto &i : paramInfo()) {
		if (!i.tune || i.type != ParamType::Float)
			continue;
		auto it = m.base.find(&i);
		g_settings->setFloat(i.setting, it != m.base.end() ? it->second : i.def);
	}
	g_settings->set("fx_mood_applied", name);
	infostream << "fx: applied mood " << name << std::endl;
}

std::vector<std::string> listMoods()
{
	std::vector<std::string> out;
	for (bool user : {false, true}) {
		for (const auto &e : fs::GetDirListing(moodDir(user))) {
			if (e.dir || !str_ends_with(e.name, ".conf") || str_starts_with(e.name, "_"))
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
	Renderer *r = s_renderer;
	std::string name;
	for (char c : raw_name)
		if (std::isalnum((unsigned char)c) || c == '_' || c == '-')
			name += c;
	if (name.empty() || !r)
		return "";
	fs::CreateAllDirs(moodDir(true));
	const std::string path = moodDir(true) + DIR_DELIM + name + ".conf";
	std::ostringstream os;
	os << "# fx mood saved from the in-game tuner\n";
	for (const auto &i : paramInfo())
		if (i.tune && i.type == ParamType::Float)
			os << i.setting << " = " << getFloat(r->base, i) << "\n";
	for (const auto &[sit, m] : r->mood.targets)
		for (const auto &i : paramInfo()) {
			auto it = m.find(&i);
			if (it != m.end())
				os << sit << "." << i.setting << " = " << it->second << "\n";
		}
	if (!fs::safeWriteToFile(path, os.str()))
		return "";
	r->mood.name = name;
	g_settings->set("fx_mood_applied", name); // already what the settings hold
	g_settings->set("fx_mood", name);
	return path;
}

void Renderer::updateMood(float dtime, const std::map<std::string, float> &active)
{
	// ~1.5 s exponential smoothing, like an eye adapting
	const float k = 1.f - std::exp(-dtime / 1.5f);
	for (const auto &[sit, map] : mood.targets) {
		auto it = active.find(sit);
		float &w = weights[sit];
		w += ((it != active.end() ? it->second : 0.f) - w) * k;
	}
	// blend order: the fixed situations, biomes, then the portal on top
	std::vector<std::pair<const ParamMap *, float>> order;
	auto add = [&] (const std::string &sit) {
		auto it = mood.targets.find(sit);
		if (it != mood.targets.end() && weights[sit] > 1e-3f)
			order.emplace_back(&it->second, weights[sit]);
	};
	for (const auto &sit : situations())
		if (sit != "portal")
			add(sit);
	for (const auto &[sit, map] : mood.targets)
		if (str_starts_with(sit, "biome."))
			add(sit);
	add("portal");

	params = base;
	for (const auto &i : paramInfo()) {
		if (!i.tune || i.type != ParamType::Float)
			continue;
		float v = getFloat(base, i);
		for (const auto &[m, w] : order) {
			auto it = m->find(&i);
			if (it != m->end())
				v += (it->second - v) * w;
		}
		setFloat(params, i, v);
	}
}

void requestTuner()
{
	if (s_renderer)
		s_renderer->tuner_requested = true;
}

bool consumeTunerRequest()
{
	return s_renderer && s_renderer->tuner_requested.exchange(false);
}

std::string tunerNote()
{
	if (!s_renderer)
		return "";
	if (!s_renderer->caps.enabled)
		return s_renderer->caps.why_disabled;
	return s_renderer->note;
}

void gradeSunlight(float rgb[3], float tod)
{
	const float amount = caps().enabled ? params().sky_tint : 0.f;
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

/*
	Per frame
*/

void noteBlockChanged(v3s16 blockpos)
{
	Renderer *r = renderer();
	if (r && r->volume)
		r->volume->noteBlockChanged(blockpos);
}

video::SColor tintParticleLight(v3s16 node, video::SColor color, float artificial)
{
	Renderer *r = renderer();
	u8 l[3];
	if (!r || !r->volume || artificial <= 0.f || !r->volume->sampleLevels(node, l))
		return color;
	const float m = std::max({l[0], l[1], l[2]});
	if (m <= 0.f)
		return color;
	// as fxArtificialLight: chroma at the luminance of white light
	float c[3] = {l[0] / m, l[1] / m, l[2] / m};
	const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
	const float k = std::clamp(r->params.light_saturation * artificial, 0.f, 1.5f);
	u32 out[3];
	const u32 in[3] = {color.getRed(), color.getGreen(), color.getBlue()};
	for (int i = 0; i < 3; i++) {
		const float ch = std::min(c[i] * (1.f / std::max(lum, 0.05f)), 2.4f);
		out[i] = (u32)std::clamp(in[i] * (1.f + (ch - 1.f) * k), 0.f, 255.f);
	}
	return video::SColor(color.getAlpha(), out[0], out[1], out[2]);
}

bool lightVolumeCurrent()
{
	Renderer *r = renderer();
	return !r || !r->volume || r->volume->current();
}

void updateFrame(Client *client, Camera *camera, Sky *sky, float dtime)
{
	Renderer *r = s_renderer;
	if (!r)
		return;
	if (r->mood_dirty.exchange(false)) {
		const std::string want = g_settings->get("fx_mood");
		if (want != g_settings->get("fx_mood_applied"))
			r->applyMood(want); // sets settings: flags settings_dirty
	}
	if (r->settings_dirty.exchange(false))
		r->onSettingsChanged();
	if (!r->caps.enabled)
		return;

	// the solid pass must not see last frame's scene copy as valid
	r->copy.valid = false;
	r->updateWorld(client, dtime);
	r->updateCamera(client, camera, sky, dtime);
	if (r->volume)
		r->volume->update(client, floatToInt(camera->getPosition(), BS), dtime);
	r->updateLights(client, camera);
	r->uploadParams();
	r->uploadFrame();
	r->pollHotReload(dtime);
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

void Renderer::pollHotReload(float dtime)
{
	if (!base.shader_hot_reload) {
		reload_primed = false;
		return;
	}
	reload_timer += dtime;
	if (reload_timer < 0.5f)
		return;
	reload_timer = 0.f;

	auto now = latestShaderMtime();
	if (!reload_primed) {
		shader_mtime = now;
		reload_primed = true;
		infostream << "fx: shader hot reload armed" << std::endl;
		return;
	}
	if (now != shader_mtime) {
		shader_mtime = now;
		infostream << "fx: shader change detected, hot reloading" << std::endl;
		shsrc->hotReloadShaders();
	}
}

} // namespace fx
