// Luanti "fx" fork: the mood renderer's interface to the engine
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Everything the upstream hooks call is declared here. All state lives in one
// fx::Renderer per world (renderer.h), created by attach() when the client
// has its media and destroyed by detach(). Outside a world every query
// answers "fx off".
//
// Client-only: nothing here touches map serialization, the network protocol
// or ContentFeatures serialization, which keeps the fork a rebaseable patch
// queue.

#pragma once

#include "caps.h"
#include "params.h"
#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <SColor.h>
#include <dimension2d.h>
#include <EMaterialTypes.h>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class ITextureSource;
class IWritableShaderSource;
class Client;
class Camera;
class Sky;
struct ContentFeatures;
struct MeshMakeData;
namespace video { class IImage; class IVideoDriver; class ITexture; class SMaterial; }
namespace scene { class ICameraSceneNode; }

namespace fx
{

/*
	Texture slots. GLSL names them in fx_common/uniforms.glsl.
*/
constexpr int TEXTURE_LAYER_FX = 2;           // emissive companion (per tile)
constexpr int TEXTURE_LAYER_SCENE_COLOR = 4;  // opaque scene copy (transparent pass)
constexpr int TEXTURE_LAYER_SCENE_DEPTH = 5;
constexpr int TEXTURE_LAYER_LIGHT_VOLUME = 6; // RGB light volume (all map materials)
constexpr int TEXTURE_LAYER_MATERIAL = 7;     // LabPBR maps: layer 2i = _n, 2i+1 = _s

/// Uniform block binding points (0 is upstream's joint matrices).
constexpr u32 UBO_PARAMS = 1, UBO_FRAME = 2, UBO_LIGHTS = 3;

/// Max lights per frame (FxLights block).
constexpr int MAX_LIGHTS = 128;

/*
	Lifecycle (Game)
*/

/// Media received, driver up: work out the caps, build the renderer, register
/// the shader constants and uniform blocks. Before any map shader is built.
void attach(video::IVideoDriver *driver, IWritableShaderSource *shsrc);
/// Leaving the world (after the client and its mesh threads are gone).
void detach();

/// The frozen per-world feature set; all off outside a world.
const Caps &caps();
/// Effective parameters this frame (settings + mood blend). Main thread.
const Params &params();

/// The client changed a map block (new data or a node edit). Main thread.
void noteBlockChanged(v3s16 blockpos);

/// No light volume, or its texture matches the map (test mode waits for it).
bool lightVolumeCurrent();

/// Once per frame before drawing: settings, moods, camera, lights, light
/// volume, uniform buffers, shader hot reload.
void updateFrame(Client *client, Camera *camera, Sky *sky, float dtime);

/*
	Moods and the tuner (game_formspec)
*/
std::vector<std::string> listMoods();
/// Save the current tunables (and the mood's situational targets) as
/// <user dir>/fx_moods/<name>.conf. Returns the path, or "" on failure.
std::string saveMood(const std::string &name);
void requestTuner();
bool consumeTunerRequest();
/// A one-line note for the tuner, e.g. settings that wait for a restart.
std::string tunerNote();
/// The tuner formspec, and its field handler (true: rebuild the form).
std::string tunerFormspec(const std::string &status);
bool tunerFields(const std::unordered_map<std::string, std::string> &fields, std::string &status);

/// Time-of-day grading of the sun/moon light colour (fx_sky_tint).
void gradeSunlight(float rgb[3], float time_of_day);

/*
	Textures and materials (node_visuals, imagesource, content_cao)
*/

/// Texture string of the emissive companion of `image`: every top-level
/// `foo.png` atom becomes `foo_e.png` when the pack has one, otherwise it is
/// blackened (alpha kept). With nothing authored and `auto_emissive`, the base
/// passes through `[fx_autoemissive`. `size` forces the companion's size.
std::string companionName(ITextureSource *tsrc, const std::string &image,
		bool auto_emissive, core::dimension2du size = {0, 0});

/// Emissive companion for an entity texture, or null without an authored map.
video::ITexture *entityEmissive(ITextureSource *tsrc, const std::string &image);

/// LabPBR `<stem>_n.png` / `<stem>_s.png` for the base atom of `image`,
/// resized to `size`, neutral fills where missing. False if neither exists.
bool materialLayers(ITextureSource *tsrc, const std::string &image,
		core::dimension2du size, std::string &normal, std::string &specular);

/// Shared neutral material array for bunches without authored maps.
video::ITexture *neutralMaterialTexture(ITextureSource *tsrc);

/// `[fx_autoemissive:<threshold>`: keep pixels brighter than the threshold.
void applyAutoEmissive(video::IImage *image, float threshold);

struct LightStyle {
	video::SColor color = video::SColor(255, 255, 255, 255);
	float flicker = 0.f; // 0..1, scaled by fx_flicker
};

/// Light colour of a light-emitting node: pack table, then its texture. The
/// colour comes from its first tile in `glowing` (tiles unique to light
/// sources), else its first non-empty tile; an authored `_e.png` wins over
/// `[fx_autoemissive`.
LightStyle resolveLightStyle(ITextureSource *tsrc, const ContentFeatures &f,
		const std::unordered_set<std::string> &glowing);

/// Warn about pack table keys that matched no node (after all nodes resolved).
void reportUnusedPackKeys();

enum class SurfaceKind : u8 { None = 0, Glass = 1, Water = 2 };
enum class SurfacePattern : u8 { None = 0, Fluted = 1, Frosted = 2, Rippled = 3 };
struct Surface {
	SurfaceKind kind = SurfaceKind::None;
	SurfacePattern pattern = SurfacePattern::None;
};

/// Glass/water surface of a node: pack table, then heuristics.
Surface resolveSurface(const ContentFeatures &f);

/// The fx post pass draws being inside this node (water): skip upstream's
/// flat post_effect_color overlay.
bool handlesUnderwater(const ContentFeatures &f);

/*
	Drawing (render pipeline, ClientMap)
*/
void applyJitter(scene::ICameraSceneNode *camera, core::dimension2du target_size);
void removeJitter(scene::ICameraSceneNode *camera);
/// Start of ClientMap's transparent pass: copy the opaque colour + depth.
void captureScene(video::IVideoDriver *driver);
/// Transparent pass, per material: bind the scene copy; fx surfaces (see
/// noteSurfaceShader) are drawn opaque with depth writes, since they
/// composite what is behind themselves.
void bindSceneCopy(video::SMaterial &material);
/// node_visuals: this material type is a glass/water surface shader.
void noteSurfaceShader(video::E_MATERIAL_TYPE type);
void unbindSceneCopy(video::SMaterial &material);
void bindLightVolume(video::SMaterial &material);

/*
	Meshgen (worker threads)
*/

/// A light or a cluster of same-coloured emitters near each other.
struct Light {
	v3f pos;          // centroid, node units, world space
	u8 level;         // max light_source in the cluster
	u16 count;        // emitters merged
	float extent;     // cluster radius in nodes
	video::SColor color;
	float flicker;
};
constexpr s16 LIGHT_CLUSTER_CELL = 4;

void collectBlockLights(const MeshMakeData *data, std::vector<Light> &out);

/*
	Shaders (ShaderSource)
*/

/// Source of `#include "fx_generated/<name>"`.
std::string generatedShaderInclude(const std::string &path);

} // namespace fx
