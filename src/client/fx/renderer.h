// Luanti "fx" fork: per-world renderer state (internal to src/client/fx)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// One Renderer exists between fx::attach() and fx::detach(). It owns every
// piece of fx state; nothing in the module keeps mutable statics, so a second
// world starts clean and no GPU resource outlives its driver/TextureSource.
//
// Threads: the renderer is used from the main thread, except caps (immutable)
// and the pack tables, which meshgen reads (built before meshing starts).

#pragma once

#include "fx.h"
#include "light_volume.h"

#include <atomic>
#include <map>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <matrix4.h>

namespace video { class IRenderTarget; }

namespace fx { class ParticleSystem; class WaterField; }

namespace fx
{

/// Mood: a preset of tunables plus situational targets.
using ParamMap = std::unordered_map<const ParamInfo *, float>;

struct Mood {
	std::string name;
	ParamMap base;   // missing = registry default
	// situation ("night", "nether", "biome.CrimsonForest", ...) -> targets the
	// renderer blends towards while that situation holds (see situations())
	std::map<std::string, ParamMap> targets;
};

/// Situations a mood can target, in blend order (later ones win). Biomes
/// ("biome.<name>") come after "end", "portal" last.
const std::vector<std::string> &situations();

/// What the companion server mod (util/fx/mods/fx_hints) tells the client.
struct Hints {
	bool present = false;
	std::string dim = "overworld", biome, weather = "none", portal = "none";
	int flash = 0;
	std::string raw; // the last text parsed
};

/// std140 mirror of FxFrame in fx_common/uniforms.glsl.
struct FrameBlock {
	float view_proj[16];
	float inv_view_proj[16];
	float prev_view_proj[16];
	float eye_time[4];          // eye (scene space), seconds
	float sun[4];               // direction towards the sun, strength
	float near_far_jitter[4];   // near, far, jitter x, jitter y (NDC)
	float volume[4];            // light volume origin (scene space), valid
	float misc[4];              // underwater, scene copy valid, TAA reset, frame
	float offset_delta[4];      // camera offset change (scene units), proj scale
	float water[4];             // surface height above the camera (scene space), -, -, -
	float events[4];            // lightning flash, damage, low health, portal
	float world[4];             // nether, end, wet (rain/storm), storm
	float weather[4];           // snow cover, raining now, wind strength, wind angle
	float fog[4];               // fog colour (display-encoded), camera offset y (scene units)
	float water_style[4];       // water scatter tint (linear rgb), choppiness
	float sky[4];               // cold (aurora), camera droplets, overworld, is day
	float cloud[4];             // height, thickness (nodes), density 0..1, visible
	float cloud_origin[4];      // noise origin x, z; camera offset x, z (world, BS units)
	float cloud_color[4];       // lit colour now (linear)
	float cloud_shadow[4];      // shadow tint (linear)
};
static_assert(sizeof(FrameBlock) == 3 * 64 + 17 * 16, "FxFrame std140 layout");

/// std140 mirror of FxLights.
struct LightsBlock {
	float pos_radius[MAX_LIGHTS * 4];  // xyz scene space, w radius (BS)
	float color_seed[MAX_LIGHTS * 4];  // rgb, w = flicker amplitude (int part /100) + seed
	s32 info[4];                       // count, held index, held level
};

struct SceneCopy {
	video::IRenderTarget *rt = nullptr;
	video::ITexture *color = nullptr;
	video::ITexture *depth = nullptr;
	core::dimension2du size;
	video::ECOLOR_FORMAT color_format = video::ECF_UNKNOWN;
	video::ECOLOR_FORMAT depth_format = video::ECF_UNKNOWN;
	bool valid = false;
};

struct CameraState {
	core::matrix4 view_proj, inv_view_proj, view_proj_unjittered, prev_view_proj;
	core::matrix4 saved_projection;
	bool jitter_applied = false;
	v3s16 offset, prev_offset;
	float jitter[2] = {};
	u32 frame = 0, frames_since_reset = 0;
	core::dimension2du target_size;
	float eye[3] = {}, offset_delta[3] = {};
	float near_far[2] = {1.f, 1000.f};
	float sun_dir[3] = {0.f, 1.f, 0.f};
	float sun_strength = 1.f;
	float underwater = 0.f;
	float water_level = 0.f;   // scene space y of the water surface above the camera
	float time = 0.f;
	float fog_color[3] = {0.7f, 0.8f, 1.f};
	bool is_day = true; // fxSunDir is the sun (not the moon)
	float proj_scale = 1.f;
};

/// One table entry; `used` feeds the unmatched-key report.
template <typename T>
struct PackEntry {
	T value;
	std::string where;         // file:line, for diagnostics
	bool heuristic_flicker = false; // light, format >= 2, no flicker= given
	mutable bool used = false;
};

/// A table keyed three ways (pack format v2): exact node name, glob over
/// node names, `@texture` (base atom of the first non-empty tile).
template <typename T>
struct PackTable {
	std::unordered_map<std::string, PackEntry<T>> exact;
	std::vector<std::pair<std::string, PackEntry<T>>> globs; // longest first
	std::unordered_map<std::string, PackEntry<T>> textures;  // "foo.png"
};

/// Surface kind from the pack table and heuristics, whether or not surfaces
/// are drawn (the light volume tints by glass either way).
Surface detectSurface(const ContentFeatures &f);

/// Pack-table glob: `*` any run, `?` one character, whole name (test_fx).
bool packGlobMatch(const std::string &pattern, const std::string &name);

/// Texture-pack tables (fx_pack.txt, fx_light_colors.txt, fx_materials.txt).
struct PackTables {
	PackTable<LightStyle> lights;
	PackTable<Surface> surfaces;
};

class Renderer
{
public:
	Renderer(video::IVideoDriver *driver, IWritableShaderSource *shsrc);
	~Renderer();

	video::IVideoDriver *driver;
	IWritableShaderSource *shsrc;

	Caps caps;
	Params base;    // straight from the settings
	Params params;  // base + situational mood blend, what the shaders see
	PackTables pack;

	// settings changes arrive on any thread; applied on the main thread
	std::atomic<bool> settings_dirty{false};
	std::atomic<bool> mood_dirty{false};
	std::atomic<bool> tuner_requested{false};
	Params last_shader_params;   // SHADERS-apply values the shaders were built with
	std::string note;            // tuner note (restart pending, ...)

	Mood mood;
	std::map<std::string, float> weights;  // situation -> smoothed weight

	// the world as the server describes it (fx_hints), and camera events
	Hints hints;
	struct Events {
		float flash = 0.f, damage = 0.f, low_health = 0.f, portal = 0.f;
		float nether = 0.f, end = 0.f, wet = 0.f, storm = 0.f;
		float heat = 0.f; // Nether, near lava
		float snow_cover = 0.f, raining = 0.f, wind = 0.5f, wind_angle = 0.7f;
		float water[4] = {0.10f, 0.45f, 0.55f, 0.5f}; // tint, choppiness (smoothed)
		float cold = 0.f, droplets = 0.f, overworld = 1.f;
		bool was_underwater = false;
		int last_flash = -1;
		s32 last_hp = -1;
		float flash_age = 10.f;
	} events;

	CameraState cam;
	CloudState clouds;
	u32 clouds_frame = 0;          // cam.frame of the last setClouds
	SceneCopy copy;
	LightsBlock lights{};
	std::unique_ptr<LightVolume> volume;
	video::ITexture *neutral_material = nullptr;
	std::unordered_set<int> surface_materials; // material types of fx surface shaders
	std::unique_ptr<ParticleSystem> particles; // GPU particles (caps.gpu_particles)
	std::unique_ptr<WaterField> water_field;   // shores and openness for the water (surfaces + volume)
	video::ITexture *saved_layer7 = nullptr;   // bindSceneCopy swaps the water field in

	// shader hot reload
	float reload_timer = 0.f;
	std::filesystem::file_time_type shader_mtime{};
	bool reload_primed = false;

	void onSettingsChanged();
	void applyMood(const std::string &name);
	void updateMood(float dtime, const std::map<std::string, float> &active);
	/// fx_hints, health, lightning, portals: per frame, before updateCamera.
	void updateWorld(Client *client, float dtime);
	void updateCamera(Client *client, Camera *camera, Sky *sky, float dtime);
	void updateLights(Client *client, Camera *camera);
	void uploadParams();
	void uploadFrame();
	void pollHotReload(float dtime);
};

/// The current world's renderer, or null.
Renderer *renderer();

// pack.cpp
/// Read the tables from `dirs` (default: the texture dirs, first wins).
void loadPackTables(PackTables &out);
void loadPackTables(PackTables &out, const std::vector<std::string> &dirs);
/// Table lookups without a renderer (test_fx); null when no key matches.
const LightStyle *packFindLight(const PackTables &t, const ContentFeatures &f, bool *heuristic_flicker = nullptr);
const Surface *packFindSurface(const PackTables &t, const ContentFeatures &f);

// shaders.cpp
void registerShaderSetters(Renderer &r);

// fx.cpp
bool loadMood(const std::string &name, Mood &out);

} // namespace fx
