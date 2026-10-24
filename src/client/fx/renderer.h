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
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <matrix4.h>

namespace video { class IRenderTarget; }

namespace fx
{

/// Mood: a preset of tunables plus situational targets.
struct Mood {
	std::string name;
	std::unordered_map<const ParamInfo *, float> base;   // missing = registry default
	std::unordered_map<const ParamInfo *, float> cave, night, underwater;
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
};
static_assert(sizeof(FrameBlock) == 3 * 64 + 7 * 16, "FxFrame std140 layout");

/// std140 mirror of FxLights.
struct LightsBlock {
	float pos_radius[MAX_LIGHTS * 4];  // xyz scene space, w radius (BS)
	float color_seed[MAX_LIGHTS * 4];  // rgb, w = flicker amplitude (int part /100) + seed
	s32 info[4];                       // count, held index
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
	float w_cave = 0.f, w_night = 0.f, w_underwater = 0.f;

	CameraState cam;
	SceneCopy copy;
	LightsBlock lights{};
	std::unique_ptr<LightVolume> volume;
	video::ITexture *neutral_material = nullptr;
	std::unordered_set<int> surface_materials; // material types of fx surface shaders

	// shader hot reload
	float reload_timer = 0.f;
	std::filesystem::file_time_type shader_mtime{};
	bool reload_primed = false;

	void onSettingsChanged();
	void applyMood(const std::string &name);
	void updateMood(float dtime, float cave, float night, float underwater);
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
