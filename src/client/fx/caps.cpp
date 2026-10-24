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
	c.surfaces = p.surfaces && c.post;
	c.colored_light = p.colored_light;
	c.light_volume = p.light_volume && p.colored_light;
	c.array_textures = driver->queryFeature(video::EVDF_TEXTURE_2D_ARRAY);
	c.materials = p.materials && c.array_textures;

	infostream << "fx: caps post=" << c.post << " hdr=" << c.hdr << " taa=" << c.taa
		<< " ao=" << c.ao << " surfaces=" << c.surfaces << " colored_light=" << c.colored_light
		<< " light_volume=" << c.light_volume << " materials=" << c.materials << std::endl;
	if (p.hdr_composite && !c.hdr)
		warningstream << "fx: HDR composite needs post-processing and float render "
			"targets; highlights will clip" << std::endl;
	return c;
}

} // namespace fx
