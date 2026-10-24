// Luanti "fx" fork: capabilities, decided once per world
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "caps.h"

#include "log.h"
#include "settings.h"
#include <IVideoDriver.h>

namespace fx
{

Caps computeCaps(const Params &p, video::IVideoDriver *driver)
{
	Caps c;
	if (!p.enable) {
		c.why_disabled = "fx_enable is off";
		return c;
	}
	// The fx shaders need GLSL 1.50 (uniform blocks, textureSize, texture
	// arrays in the vertex stage). GLES and the legacy OpenGL 2 driver run
	// plain upstream rendering instead.
	if (!driver || driver->getDriverType() != video::EDT_OPENGL3) {
		c.why_disabled = "fx needs video_driver = opengl3";
		warningstream << "fx: disabled, " << c.why_disabled << std::endl;
		return c;
	}
	c.enabled = true;

	// mirror render/secondstage.cpp's decisions
	c.post = g_settings->getBool("enable_post_processing");
	const bool float_rt = driver->queryFeature(video::EVDF_RENDER_TO_FLOAT_TEXTURE);
	const std::string aa = g_settings->get("antialiasing");
	c.msaa = c.post && aa == "fsaa" && driver->queryFeature(video::EVDF_TEXTURE_MULTISAMPLE);
	c.ssaa = c.post && aa == "ssaa";
	// the pipeline then picks a float scene buffer (render/plain.cpp),
	// whatever post_processing_texture_bits says
	c.hdr = c.post && float_rt && p.hdr_composite &&
			driver->queryTextureFormat(video::ECF_A16B16G16R16F);

	c.taa = p.taa && c.hdr && !c.msaa && !c.ssaa;
	c.ao = p.ao && c.hdr; // applied in the linear HDR composite
	c.volumetrics = p.use_volumetrics && c.hdr;
	c.gi = p.use_gi && c.hdr;
	c.clouds = p.use_clouds && c.hdr;
	c.surfaces = p.surfaces && c.post;
	c.colored_light = p.colored_light;
	c.light_volume = c.colored_light; // the volume is how light gets its colour
	c.array_textures = driver->queryFeature(video::EVDF_TEXTURE_2D_ARRAY);
	c.materials = p.materials && c.array_textures;
	// GPU particles: RGBA32F state targets, the light volume's node snapshot
	// for collision and emitters, an array texture for leaf tiles. Multiple
	// render targets are core in GL 3.3 (Irrlicht's render target uses
	// GL_MAX_DRAW_BUFFERS; its EVDF_MULTIPLE_RENDER_TARGETS answer is a
	// hard-coded false), so they aren't queried.
	c.gpu_particles = p.use_gpu_particles && c.light_volume && float_rt &&
			c.array_textures && driver->queryTextureFormat(video::ECF_A32B32G32R32F);
	if (p.use_gpu_particles && !c.gpu_particles)
		warningstream << "fx: GPU particles need fx_colored_light and float render "
			"targets; off" << std::endl;

	infostream << "fx: caps post=" << c.post << " hdr=" << c.hdr << " taa=" << c.taa
		<< " ao=" << c.ao << " surfaces=" << c.surfaces << " colored_light=" << c.colored_light
		<< " light_volume=" << c.light_volume << " materials=" << c.materials
		<< " gpu_particles=" << c.gpu_particles << std::endl;
	if (p.hdr_composite && !c.hdr)
		warningstream << "fx: HDR composite needs post-processing and float render "
			"targets; highlights will clip" << std::endl;
	return c;
}

} // namespace fx
