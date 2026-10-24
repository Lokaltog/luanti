// Luanti "fx" fork: camera state, TAA jitter, scene copy, frame uniforms
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "renderer.h"

#include "client/camera.h"
#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/clientmap.h"
#include "client/sky.h"
#include "log.h"
#include "nodedef.h"
#include <ICameraSceneNode.h>
#include <IRenderTarget.h>
#include <IVideoDriver.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx
{

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

void Renderer::updateCamera(Client *client, Camera *camera, Sky *sky, float dtime)
{
	auto *node = camera->getCameraNode();
	cam.near_far[0] = node->getNearValue();
	cam.near_far[1] = node->getFarValue();

	// The engine's own sun and moon (orbit tilt, server sky params), like the
	// shadow renderer uses.
	const float tod = client->getEnv().getTimeOfDayF();
	const float wicked = getWickedTimeOfDay(tod); // what the sky and shadows use
	const bool day = wicked > 0.25f && wicked < 0.75f;
	v3f sun = sky ? (day ? sky->getSunDirection() : sky->getMoonDirection()) :
			v3f(std::cos((tod - 0.25f) * 2.f * core::PI), std::sin((tod - 0.25f) * 2.f * core::PI), 0.15f);
	sun.normalize();
	cam.sun_dir[0] = sun.X;
	cam.sun_dir[1] = sun.Y;
	cam.sun_dir[2] = sun.Z;
	// 0 below the horizon, 1 well above it; moonlight counts as weak
	cam.sun_strength = std::clamp(sun.Y * 3.f, 0.f, 1.f) * (day ? 1.f : 0.25f);
	if (sky) {
		const video::SColor fc = sky->getFogColor();
		cam.fog_color[0] = fc.getRed() / 255.f;
		cam.fog_color[1] = fc.getGreen() / 255.f;
		cam.fog_color[2] = fc.getBlue() / 255.f;
	}

	ClientMap &map = client->getEnv().getClientMap();
	const v3f cam_pos = camera->getPosition();
	const MapNode n = map.getNode(floatToInt(cam_pos, BS));
	const NodeDefManager *ndef = client->ndef();
	const v3s16 cam_node = floatToInt(cam_pos, BS);
	cam.underwater = resolveSurface(ndef->get(n)).kind == SurfaceKind::Water ? 1.f : 0.f;
	if (cam.underwater > 0.f) {
		// the surface: top of the water column the camera is in (full nodes;
		// flowing water's lower level is ignored)
		v3s16 p = cam_node;
		for (int i = 0; i < 64; i++) {
			const MapNode above = map.getNode(p + v3s16(0, 1, 0));
			if (resolveSurface(ndef->get(above)).kind != SurfaceKind::Water)
				break;
			p.Y++;
		}
		cam.water_level = (p.Y + 0.5f) * BS - camera->getOffset().Y * BS;
	}

	// mood: cave = little sky light reaching the camera (the day bank holds
	// sky light, day and night), night = day/night ratio
	const float skylight = n.getLight(LIGHTBANK_DAY, ndef->getLightingFlags(n));
	const float cave = std::clamp((11.f - skylight) / 8.f, 0.f, 1.f);
	const float dnr = client->getEnv().getDayNightRatio() / 1000.f;
	const float night = std::clamp((0.6f - dnr) / 0.4f, 0.f, 1.f);
	std::map<std::string, float> active = {{"cave", cave}, {"night", night},
		{"underwater", cam.underwater}, {"nether", events.nether}, {"end", events.end},
		{"rain", hints.weather == "rain" ? 1.f : 0.f},
		{"snow", hints.weather == "snow" ? 1.f : 0.f},
		{"storm", hints.weather == "thunder" ? 1.f : 0.f},
		{"heat", events.heat}, {"portal", events.portal},
		// dawn: sunrise and the hour after (time of day 0.2 .. 0.32)
		{"dawn", std::clamp(1.f - std::abs(tod - 0.26f) / 0.07f, 0.f, 1.f)}};
	if (!hints.biome.empty())
		active["biome." + hints.biome] = 1.f;
	// weather is outdoors: under a roof (the cave measure) it fades out
	for (const char *w : {"rain", "snow", "storm"})
		active[w] *= 1.f - cave;
	updateMood(dtime, active);
	cam.time = std::fmod(cam.time + dtime, 3600.f);

	// TAA bookkeeping: last frame's unjittered matrix becomes "previous"
	cam.prev_view_proj = cam.view_proj_unjittered;
	cam.prev_offset = cam.offset;
	cam.offset = camera->getOffset();
	const v3f eye = cam_pos - intToFloat(cam.offset, BS);
	cam.eye[0] = eye.X;
	cam.eye[1] = eye.Y;
	cam.eye[2] = eye.Z;
	const v3s16 d = cam.offset - cam.prev_offset;
	cam.offset_delta[0] = d.X * BS;
	cam.offset_delta[1] = d.Y * BS;
	cam.offset_delta[2] = d.Z * BS;
	if (d != v3s16(0, 0, 0) || cam.frame == 0)
		cam.frames_since_reset = 0;
	cam.frame++;
	cam.frames_since_reset++;
}

void Renderer::uploadFrame()
{
	FrameBlock b{};
	std::memcpy(b.view_proj, cam.view_proj.pointer(), sizeof(b.view_proj));
	std::memcpy(b.inv_view_proj, cam.inv_view_proj.pointer(), sizeof(b.inv_view_proj));
	std::memcpy(b.prev_view_proj, cam.prev_view_proj.pointer(), sizeof(b.prev_view_proj));
	b.eye_time[0] = cam.eye[0];
	b.eye_time[1] = cam.eye[1];
	b.eye_time[2] = cam.eye[2];
	b.eye_time[3] = cam.time;
	b.sun[0] = cam.sun_dir[0];
	b.sun[1] = cam.sun_dir[1];
	b.sun[2] = cam.sun_dir[2];
	b.sun[3] = cam.sun_strength;
	b.near_far_jitter[0] = cam.near_far[0];
	b.near_far_jitter[1] = cam.near_far[1];
	b.near_far_jitter[2] = cam.jitter[0];
	b.near_far_jitter[3] = cam.jitter[1];
	if (volume && volume->valid()) {
		const v3s16 o = volume->origin();
		b.volume[0] = (o.X - cam.offset.X) * BS;
		b.volume[1] = (o.Y - cam.offset.Y) * BS;
		b.volume[2] = (o.Z - cam.offset.Z) * BS;
		b.volume[3] = 1.f;
	}
	b.misc[0] = cam.underwater;
	b.misc[1] = copy.valid ? 1.f : 0.f;
	b.misc[2] = cam.frames_since_reset < 2 ? 1.f : 0.f;
	b.misc[3] = (float)(cam.frame % 1024);
	b.offset_delta[0] = cam.offset_delta[0];
	b.offset_delta[1] = cam.offset_delta[1];
	b.offset_delta[2] = cam.offset_delta[2];
	b.offset_delta[3] = cam.proj_scale;
	b.water[0] = cam.water_level;
	b.events[0] = events.flash;
	b.events[1] = events.damage;
	b.events[2] = events.low_health;
	b.events[3] = events.portal;
	b.world[0] = events.nether;
	b.world[1] = events.end;
	b.world[2] = events.wet;
	b.world[3] = events.storm;
	b.weather[0] = events.snow_cover;
	b.weather[1] = events.raining;
	b.weather[2] = events.wind;
	b.weather[3] = events.wind_angle;
	b.fog[0] = cam.fog_color[0];
	b.fog[1] = cam.fog_color[1];
	b.fog[2] = cam.fog_color[2];
	b.fog[3] = cam.offset.Y * BS;
	driver->setUniformBufferData(UBO_FRAME, &b, sizeof(b));
}

void Renderer::uploadParams()
{
	static thread_local std::vector<float> block;
	packParamBlock(params, block);
	driver->setUniformBufferData(UBO_PARAMS, block.data(), block.size() * sizeof(float));
}

/*
	TAA jitter, only when the pipeline resolves it (caps.taa)
*/

void applyJitter(scene::ICameraSceneNode *camera, core::dimension2du target_size)
{
	Renderer *r = renderer();
	if (!r)
		return;
	CameraState &c = r->cam;
	c.jitter[0] = c.jitter[1] = 0.f;
	if (!r->caps.taa || !camera || !target_size.Width)
		return;
	if (target_size != c.target_size) {
		c.target_size = target_size;
		c.frames_since_reset = 0;
	}
	// 8-sample Halton(2,3), centred on the pixel, in NDC units
	const u32 i = (c.frame % 8) + 1;
	c.jitter[0] = (halton(i, 2) - 0.5f) * 2.f / target_size.Width;
	c.jitter[1] = (halton(i, 3) - 0.5f) * 2.f / target_size.Height;
	c.saved_projection = camera->getProjectionMatrix();
	core::matrix4 p = c.saved_projection;
	// Irrlicht is left-handed with clip.w = z_view, so adding to the third
	// row's x/y shifts NDC by exactly that amount at every depth.
	p[8] += c.jitter[0];
	p[9] += c.jitter[1];
	camera->setProjectionMatrix(p, false);
	c.jitter_applied = true;
}

void removeJitter(scene::ICameraSceneNode *camera)
{
	Renderer *r = renderer();
	if (!r || !r->cam.jitter_applied || !camera)
		return;
	camera->setProjectionMatrix(r->cam.saved_projection, false);
	r->cam.jitter_applied = false;
}

/*
	Scene copy, at the start of the transparent pass
*/

void captureScene(video::IVideoDriver *driver)
{
	Renderer *r = renderer();
	if (!r || !r->caps.enabled)
		return;
	CameraState &c = r->cam;
	SceneCopy &copy = r->copy;
	copy.valid = false;

	// the 3D transforms are still set: record the matrices for the
	// transparent pass and the post passes
	core::matrix4 proj = driver->getTransform(video::ETS_PROJECTION);
	const core::matrix4 &view = driver->getTransform(video::ETS_VIEW);
	core::matrix4 vp = proj;
	vp *= view;
	c.view_proj = vp;
	vp.getInverse(c.inv_view_proj);
	proj[8] -= c.jitter[0];
	proj[9] -= c.jitter[1];
	c.proj_scale = proj[5] * 0.5f * driver->getCurrentRenderTargetSize().Height;
	c.view_proj_unjittered = proj;
	c.view_proj_unjittered *= view;

	if (r->caps.surfaces) {
		video::IRenderTarget *src = driver->getCurrentRenderTarget();
		if (src && !src->getTexture().empty() && src->getDepthStencil()) {
			video::ITexture *src_color = src->getTexture()[0];
			video::ITexture *src_depth = src->getDepthStencil();
			const auto size = src_color->getSize();
			const auto cf = src_color->getColorFormat();
			const auto df = src_depth->getColorFormat();
			if (!copy.rt || copy.size != size || copy.color_format != cf ||
					copy.depth_format != df) {
				if (copy.rt)
					driver->removeRenderTarget(copy.rt);
				if (copy.color)
					driver->removeTexture(copy.color);
				if (copy.depth)
					driver->removeTexture(copy.depth);
				copy = SceneCopy();
				copy.color = driver->addRenderTargetTexture(size, "fx_scene_color", cf);
				copy.depth = driver->addRenderTargetTexture(size, "fx_scene_depth", df);
				copy.rt = driver->addRenderTarget();
				if (copy.color && copy.depth && copy.rt) {
					copy.rt->setTexture(copy.color, copy.depth);
					copy.size = size;
					copy.color_format = cf;
					copy.depth_format = df;
					infostream << "fx: scene copy " << size.Width << "x" << size.Height << std::endl;
				} else {
					errorstream << "fx: could not create the scene copy target" << std::endl;
				}
			}
			if (copy.size == size) {
				// glBlitFramebuffer: resolves MSAA too, restores the previous FBO
				driver->blitRenderTarget(src, copy.rt);
				copy.valid = true;
			}
		}
	}
	// fresh matrices (and scene copy flag) for everything drawn from here on
	r->uploadFrame();
}

void noteSurfaceShader(video::E_MATERIAL_TYPE type)
{
	if (Renderer *r = renderer())
		r->surface_materials.insert((int)type);
}

void bindSceneCopy(video::SMaterial &material)
{
	Renderer *r = renderer();
	if (!r || !r->copy.valid)
		return;
	if (r->surface_materials.count((int)material.MaterialType)) {
		// The surface writes alpha 1 (it composites what is behind from the
		// copy), so the alpha blend reduces to "replace". Writing depth ends
		// sorting artefacts between panes and water, and AO sees the surface
		// instead of what is behind it.
		material.ZWriteEnable = video::EZW_ON;
	}
	auto &c = material.TextureLayers[TEXTURE_LAYER_SCENE_COLOR];
	c.Texture = r->copy.color;
	c.TextureWrapU = c.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	c.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	c.MagFilter = video::ETMAGF_LINEAR;
	auto &d = material.TextureLayers[TEXTURE_LAYER_SCENE_DEPTH];
	d.Texture = r->copy.depth;
	d.TextureWrapU = d.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	d.MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
	d.MagFilter = video::ETMAGF_NEAREST;
}

void unbindSceneCopy(video::SMaterial &material)
{
	material.TextureLayers[TEXTURE_LAYER_SCENE_COLOR].Texture = nullptr;
	material.TextureLayers[TEXTURE_LAYER_SCENE_DEPTH].Texture = nullptr;
}

void bindLightVolume(video::SMaterial &material)
{
	Renderer *r = renderer();
	auto &l = material.TextureLayers[TEXTURE_LAYER_LIGHT_VOLUME];
	if (!r || !r->volume || !r->volume->valid()) {
		l.Texture = nullptr;
		return;
	}
	l.Texture = r->volume->texture();
	l.TextureWrapU = l.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	l.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	l.MagFilter = video::ETMAGF_LINEAR;
}

void adjustFilters(video::SMaterial &material)
{
	Renderer *r = renderer();
	if (!r || !r->caps.materials || !r->params.smooth_material_maps)
		return;
	auto &m = material.TextureLayers[TEXTURE_LAYER_MATERIAL];
	if (!m.Texture)
		return;
	m.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	m.MagFilter = video::ETMAGF_LINEAR;
}

bool handlesUnderwater(const ContentFeatures &f)
{
	const Caps &c = caps();
	return c.surfaces && c.hdr && resolveSurface(f).kind == SurfaceKind::Water;
}

} // namespace fx
