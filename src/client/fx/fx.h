// Luanti "fx" fork: mood renderer, milestone 1 (light & glow)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Everything in this module is client-only. It never touches map
// serialization, the network protocol or ContentFeatures serialization,
// which keeps the fork a rebaseable patch queue.

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include <SColor.h>
#include <dimension2d.h>
#include <memory>
#include <string>
#include <vector>

class ITextureSource;
class IWritableShaderSource;
class IShaderSource;
class Client;
class Camera;
struct ContentFeatures;
struct MeshMakeData;
class IShaderConstantSetter;
class IShaderUniformSetterFactory;
namespace video { class IImage; }

namespace fx
{

/// Max lights uploaded to shaders per frame. Must match FX_LIGHT_MAX in GLSL
/// (it is emitted as a shader constant, so only this value needs changing).
constexpr int MAX_LIGHTS = 128;

/// Texture slot for the per-tile FX material companion (emissive for now).
/// Slot 1 is the crack texture, slot 3 the shadow map, slot 2 was free.
constexpr int TEXTURE_LAYER_FX = 2;

/// Settings, refreshed from g_settings whenever an fx_* setting changes.
struct Params {
	bool enabled = true;           // fx_enable
	bool hdr_composite = true;     // fx_hdr_composite (needs restart: shader define)
	int tonemapper = 1;            // fx_tonemapper: 0 = upstream Hable, 1 = AgX, 2 = ACES
	float exposure = 1.5f;         // fx_exposure
	float grade_saturation = 1.3f; // fx_grade_saturation (AgX look)
	float grade_contrast = 1.2f;   // fx_grade_contrast (AgX look power)
	float bloom_threshold = 1.0f;  // fx_bloom_threshold (linear luminance)
	float bloom_knee = 0.5f;       // fx_bloom_knee
	float bloom_intensity = 0.6f;  // fx_bloom_intensity (additive)
	float emissive_strength = 4.0f;// fx_emissive_strength (HDR multiplier)
	float auto_emissive_threshold = 0.55f; // fx_auto_emissive_threshold
	bool colored_light = true;     // fx_colored_light
	float light_saturation = 0.85f;// fx_light_saturation (0 = upstream white light)
	float light_gather_radius = 48.f; // fx_light_gather_radius, in nodes
	float flicker = 0.12f;         // fx_flicker (amplitude)
	bool held_light = true;        // fx_held_light
	int debug_view = 0;            // fx_debug_view, see FX_DEBUG_* in shaders
	bool hot_reload = false;       // fx_shader_hot_reload
};

const Params &params();
/// Register settings callbacks; call once at client start.
void init();
void deinit();

/*
	Material companions (emissive maps)
*/

/// Build the texture string of the emissive companion of `image`.
/// Every `foo.png` atom is replaced by `foo_e.png` when that file exists,
/// otherwise blackened while keeping its alpha, so overlays composite right.
/// If no atom has an authored map and `auto_emissive` is set (the node is a
/// light source), the base image is passed through `[fx_autoemissive`.
/// `size` (if non-zero) forces the companion to the base texture's size.
std::string companionName(ITextureSource *tsrc, const std::string &image,
		bool auto_emissive, core::dimension2du size = {0, 0});

/// Texture modifier implementation: `[fx_autoemissive:<threshold>`.
/// Keeps pixels brighter than `threshold` (smoothstep), darkens the rest.
/// Called from ImageSource; operates in place on an ARGB8 image.
void applyAutoEmissive(video::IImage *image, float threshold);

/*
	Light colors
*/

struct LightStyle {
	video::SColor color = video::SColor(255, 255, 255, 255);
	float flicker = 0.f; // 0..1, multiplied with fx_flicker
};

/// Resolve the light color of a light-emitting node: texture pack table
/// (`fx_light_colors.txt`) first, then auto-derived from its texture.
LightStyle resolveLightStyle(ITextureSource *tsrc, const ContentFeatures &f);

/// Forget the loaded table (media reload).
void resetLightTable();

/*
	Per-block light lists, built during meshgen (worker thread, read-only after)
*/

/// A light or a cluster of same-colored emitters (lava lakes, torch walls).
/// Clustering keeps one lake from crowding every other light out of the
/// fixed-size per-frame list.
struct Light {
	v3f pos;                // centroid, node units, world space
	u8 level;               // max light_source in the cluster, 1..14
	u16 count;              // number of emitters merged
	float extent;           // cluster radius in nodes (0 for a single node)
	video::SColor color;
	float flicker;
};

/// Cluster cell size in nodes.
constexpr s16 LIGHT_CLUSTER_CELL = 4;

void collectBlockLights(const MeshMakeData *data, std::vector<Light> &out);

/*
	Per-frame state shared with the shaders
*/

struct FrameLights {
	int count = 0;
	float pos_radius[MAX_LIGHTS * 4] = {}; // xyz in BS units relative to camera offset, w = radius (BS)
	float color_seed[MAX_LIGHTS * 4] = {};  // rgb linear-ish color, a = flicker seed + amplitude packed
};

/// Gather the nearest lights around the camera from loaded block meshes,
/// plus the held item. Call once per frame before drawing.
void updateFrame(Client *client, Camera *camera, float dtime);

const FrameLights &frameLights();

std::unique_ptr<IShaderConstantSetter> createConstantSetter();
std::unique_ptr<IShaderUniformSetterFactory> createUniformSetterFactory();

/*
	Shader hot reload
*/

/// Poll shader file modification times; call once per frame.
void pollShaderHotReload(IWritableShaderSource *shsrc, float dtime);

} // namespace fx
