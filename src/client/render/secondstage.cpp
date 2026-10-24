// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2017 numzero, Lobachevskiy Vitaliy <numzer0@yandex.ru>
// Copyright (C) 2020 appgurueu, Lars Mueller <appgurulars@gmx.de>

#include "secondstage.h"
#include "client/fx/fx.h"
#include "client/client.h"
#include "client/shader.h"
#include "settings.h"
#include "plain.h"
#include <ISceneManager.h>

PostProcessingStep::PostProcessingStep(u32 _shader_id, const std::vector<u8> &_texture_map) :
	shader_id(_shader_id), texture_map(_texture_map)
{
	assert(texture_map.size() <= video::MATERIAL_MAX_TEXTURES);
	configureMaterial();
}

void PostProcessingStep::configureMaterial()
{
	material.UseMipMaps = false;
	material.ZBuffer = video::ECFN_LESSEQUAL;
	material.ZWriteEnable = video::EZW_ON;
	for (u32 k = 0; k < texture_map.size(); ++k) {
		material.TextureLayers[k].AnisotropicFilter = 0;
		material.TextureLayers[k].MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
		material.TextureLayers[k].MagFilter = video::ETMAGF_NEAREST;
		material.TextureLayers[k].TextureWrapU = video::ETC_CLAMP_TO_EDGE;
		material.TextureLayers[k].TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	}
}

void PostProcessingStep::setRenderSource(RenderSource *_source)
{
	source = _source;
}

void PostProcessingStep::setRenderTarget(RenderTarget *_target)
{
	target = _target;
}

void PostProcessingStep::reset(PipelineContext &context)
{
}

void PostProcessingStep::run(PipelineContext &context)
{
	if (target)
		target->activate(context);

	// attach the shader
	material.MaterialType = context.client->getShaderSource()->getShaderInfo(shader_id).material;

	auto driver = context.device->getVideoDriver();

	for (u32 i = 0; i < texture_map.size(); i++)
		material.TextureLayers[i].Texture = texture_map[i] == fx::RENDER_SOURCE_LIGHT_VOLUME ?
				fx::lightVolumeTexture() : source->getTexture(texture_map[i]);

	static const video::SColor color = video::SColor(0, 0, 0, 255);
	static const video::S3DVertex vertices[4] = {
			video::S3DVertex(1.0, -1.0, 0.0, 0.0, 0.0, -1.0,
					color, 1.0, 0.0),
			video::S3DVertex(-1.0, -1.0, 0.0, 0.0, 0.0, -1.0,
					color, 0.0, 0.0),
			video::S3DVertex(-1.0, 1.0, 0.0, 0.0, 0.0, -1.0,
					color, 0.0, 1.0),
			video::S3DVertex(1.0, 1.0, 0.0, 0.0, 0.0, -1.0,
					color, 1.0, 1.0),
	};
	static const u16 indices[6] = {0, 1, 2, 2, 3, 0};
	driver->setMaterial(material);
	driver->drawVertexPrimitiveList(&vertices, 4, &indices, 2);
}

void PostProcessingStep::setBilinearFilter(u8 index, bool value)
{
	assert(index < video::MATERIAL_MAX_TEXTURES);
	material.TextureLayers[index].MinFilter = value ? video::ETMINF_LINEAR_MIPMAP_NEAREST : video::ETMINF_NEAREST_MIPMAP_NEAREST;
	material.TextureLayers[index].MagFilter = value ? video::ETMAGF_LINEAR : video::ETMAGF_NEAREST;
}

RenderStep *addPostProcessing(RenderPipeline *pipeline, RenderStep *previousStep, v2f scale, Client *client)
{
	auto buffer = pipeline->createOwned<TextureBuffer>();
	auto driver = client->getSceneManager()->getVideoDriver();

	// configure texture formats
	video::ECOLOR_FORMAT color_format = selectColorFormat(driver);
	video::ECOLOR_FORMAT depth_format = selectDepthFormat(driver);

	verbosestream << "addPostProcessing(): color = "
		<< video::ColorFormatName(color_format) << ", depth = "
		<< video::ColorFormatName(depth_format) << std::endl;

	// init post-processing buffer
	static const u8 TEXTURE_COLOR = 0;
	static const u8 TEXTURE_DEPTH = 1;
	static const u8 TEXTURE_BLOOM = 2;
	static const u8 TEXTURE_EXPOSURE_1 = 3;
	static const u8 TEXTURE_EXPOSURE_2 = 4;
	static const u8 TEXTURE_FXAA = 5;
	static const u8 TEXTURE_VOLUME = 6;

	static const u8 TEXTURE_MSAA_COLOR = 7;
	static const u8 TEXTURE_MSAA_DEPTH = 8;

	static const u8 TEXTURE_SCALE_DOWN = 10;
	static const u8 TEXTURE_SCALE_UP = 20;

	// because bloom_format is floating point
	const bool bloom_available = driver->queryFeature(video::EVDF_RENDER_TO_FLOAT_TEXTURE);
	const bool enable_bloom = g_settings->getBool("enable_bloom") && bloom_available;
	const bool enable_volumetric_light = g_settings->getBool("enable_volumetric_lighting") && enable_bloom;
	const bool enable_auto_exposure = g_settings->getBool("enable_auto_exposure") && bloom_available;
	if (g_settings->getBool("enable_bloom") && !bloom_available) {
		warningstream << "Ignoring configured bloom since it's not supported by "
			"the current video driver." << std::endl;
	}
	if (g_settings->getBool("enable_auto_exposure") && !bloom_available) {
		warningstream << "Ignoring configured auto exposure since it's not supported by "
			"the current video driver." << std::endl;
	}

	verbosestream << "addPostProcessing(): bloom = "
		<< enable_bloom << (enable_volumetric_light ? " + volumetric" : "")
		<< ", exposure = " << enable_auto_exposure << std::endl;

	const std::string antialiasing = g_settings->get("antialiasing");
	const u16 antialiasing_scale = MYMAX(2, g_settings->getU16("fsaa"));

	// This code only deals with MSAA in combination with post-processing. MSAA without
	// post-processing works via a flag at OpenGL context creation instead.
	// To make MSAA work with post-processing, we need multisample texture support,
	// which has higher OpenGL (ES) version requirements.
	// Note: This is not about renderbuffer objects, but about textures,
	// since that's what we use and what Irrlicht allows us to use.

	const bool msaa_available = driver->queryFeature(video::EVDF_TEXTURE_MULTISAMPLE);
	const bool enable_msaa = antialiasing == "fsaa" && msaa_available;
	if (antialiasing == "fsaa" && !msaa_available) {
		warningstream << "Ignoring configured FSAA since it's not supported in "
			"combination with post-processing by the current video driver." << std::endl;
	}

	const bool enable_ssaa = antialiasing == "ssaa";
	const bool enable_fxaa = g_settings->getBool("fxaa");

	verbosestream << "addPostProcessing(): AA = "
		<< (enable_msaa ? "msaa" : enable_ssaa ? "ssaa" : "none")
		<< " " << antialiasing_scale << "x" << (enable_fxaa ? " + fxaa" : "") << std::endl;

	// Super-sampling is simply rendering into a larger texture.
	// Downscaling is done by the final step when rendering to the screen.
	if (enable_ssaa) {
		scale *= antialiasing_scale;
	}

	if (enable_msaa) {
		buffer->setTexture(TEXTURE_MSAA_COLOR, scale, "3d_render_msaa", color_format, false, antialiasing_scale);
		buffer->setTexture(TEXTURE_MSAA_DEPTH, scale, "3d_depthmap_msaa", depth_format, false, antialiasing_scale);
	}

	buffer->setTexture(TEXTURE_COLOR, scale, "3d_render", color_format);
	buffer->setTexture(TEXTURE_EXPOSURE_1, core::dimension2du(1,1), "exposure_1", color_format, /*clear:*/ true);
	buffer->setTexture(TEXTURE_EXPOSURE_2, core::dimension2du(1,1), "exposure_2", color_format, /*clear:*/ true);
	buffer->setTexture(TEXTURE_DEPTH, scale, "3d_depthmap", depth_format);

	// attach buffer to the previous step
	if (enable_msaa) {
		TextureBufferOutput *msaa = pipeline->createOwned<TextureBufferOutput>(buffer, std::vector<u8> { TEXTURE_MSAA_COLOR }, TEXTURE_MSAA_DEPTH);
		previousStep->setRenderTarget(msaa);
		TextureBufferOutput *normal = pipeline->createOwned<TextureBufferOutput>(buffer, std::vector<u8> { TEXTURE_COLOR }, TEXTURE_DEPTH);
		pipeline->addStep<ResolveMSAAStep>(msaa, normal);
	} else {
		previousStep->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, std::vector<u8> { TEXTURE_COLOR }, TEXTURE_DEPTH));
	}

	// shared variables
	u32 shader_id;

	// Number of mipmap levels of the bloom downsampling texture
	// (this affects the bloom strength, so don't blindly change it)
	const u8 MIPMAP_LEVELS = 4;

	// color_format can be a normalized integer format, but bloom requires
	// values outside of [0,1] so this needs to be a different one.
	const auto bloom_format = video::ECF_A16B16G16R16F;

	// post-processing stage

	/*
		fx: ambient occlusion and TAA, before anything reads the scene colour.
		AO is applied inside the TAA resolve so its noise averages out over
		frames; without TAA the final pass applies it instead.
	*/
	static const u8 TEXTURE_FX_TAA_OUT = 30;
	static const u8 TEXTURE_FX_TAA_HISTORY = 31;
	static const u8 TEXTURE_FX_AO = 32;
	const bool fx_ao = fx::caps().ao;   // decided once per world (fx/caps.cpp)
	const bool fx_taa = fx::caps().taa;
	u8 scene_color = TEXTURE_COLOR;

	if (fx_ao) {
		buffer->setTexture(TEXTURE_FX_AO, scale * 0.5f, "fx_ao", video::ECF_A8R8G8B8);
		shader_id = client->getShaderSource()->getShaderRaw("fx_ao");
		auto ao = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { TEXTURE_DEPTH });
		ao->setRenderSource(buffer);
		ao->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FX_AO));
	}
	// fx wave 3: volumetric light and height fog, bounce light (half res)
	static const u8 TEXTURE_FX_VOLUMETRIC = 34;
	static const u8 TEXTURE_FX_GI = 35;
	const bool fx_vol = fx::caps().volumetrics, fx_gi = fx::caps().gi;
	if (fx_vol) {
		buffer->setTexture(TEXTURE_FX_VOLUMETRIC, scale * 0.5f, "fx_volumetric", bloom_format);
		shader_id = client->getShaderSource()->getShaderRaw("fx_volumetric");
		auto vol = pipeline->addStep<PostProcessingStep>(shader_id,
				std::vector<u8> { TEXTURE_DEPTH, fx::RENDER_SOURCE_LIGHT_VOLUME });
		vol->setRenderSource(buffer);
		vol->setBilinearFilter(1, true); // the light volume: bilinear within a slice
		vol->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FX_VOLUMETRIC));
	}
	if (fx_gi) {
		buffer->setTexture(TEXTURE_FX_GI, scale * 0.5f, "fx_gi", bloom_format);
		shader_id = client->getShaderSource()->getShaderRaw("fx_gi");
		auto gi = pipeline->addStep<PostProcessingStep>(shader_id,
				std::vector<u8> { TEXTURE_COLOR, TEXTURE_DEPTH });
		gi->setRenderSource(buffer);
		gi->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FX_GI));
	}
	static const u8 TEXTURE_FX_CLOUDS = 36;
	const bool fx_clouds = fx::caps().clouds;
	if (fx_clouds) {
		buffer->setTexture(TEXTURE_FX_CLOUDS, scale * 0.5f, "fx_clouds", bloom_format);
		shader_id = client->getShaderSource()->getShaderRaw("fx_clouds");
		auto cl = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { TEXTURE_DEPTH });
		cl->setRenderSource(buffer);
		cl->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FX_CLOUDS));
	}
	if (fx_taa) {
		buffer->setTexture(TEXTURE_FX_TAA_OUT, scale, "fx_taa_out", bloom_format);
		buffer->setTexture(TEXTURE_FX_TAA_HISTORY, scale, "fx_taa_history", bloom_format);
		std::vector<u8> inputs { TEXTURE_COLOR, TEXTURE_DEPTH, TEXTURE_FX_TAA_HISTORY };
		if (fx_ao)
			inputs.push_back(TEXTURE_FX_AO);
		shader_id = client->getShaderSource()->getShaderRaw("fx_taa");
		auto taa = pipeline->addStep<PostProcessingStep>(shader_id, inputs);
		taa->setRenderSource(buffer);
		taa->setBilinearFilter(2, true); // history is resampled at reprojected positions
		if (fx_ao)
			taa->setBilinearFilter(3, true);
		taa->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FX_TAA_OUT));
		// ping-pong: this frame's resolve becomes next frame's history
		pipeline->addStep<SwapTexturesStep>(buffer, TEXTURE_FX_TAA_OUT, TEXTURE_FX_TAA_HISTORY);
		scene_color = TEXTURE_FX_TAA_HISTORY;
	}

	u8 source = scene_color;

	// common downsampling step for bloom or autoexposure
	if (enable_bloom || enable_auto_exposure) {

		v2f downscale = scale * 0.5f;
		for (u8 i = 0; i < MIPMAP_LEVELS; i++) {
			buffer->setTexture(TEXTURE_SCALE_DOWN + i, downscale, std::string("downsample") + std::to_string(i), bloom_format);
			if (enable_bloom)
				buffer->setTexture(TEXTURE_SCALE_UP + i, downscale, std::string("upsample") + std::to_string(i), bloom_format);
			downscale *= 0.5f;
		}

		if (enable_bloom) {
			// fx: extract at half resolution: the bloom chain blurs it
			// anyway, and a full-res float target is a lot of bandwidth at 4K
			buffer->setTexture(TEXTURE_BLOOM, fx::caps().hdr ? scale * 0.5f : scale, "bloom", bloom_format);

			// get bright spots
			u32 shader_id = client->getShaderSource()->getShaderRaw("extract_bloom");
			auto *extract_bloom = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { source, TEXTURE_EXPOSURE_1 });
			if (fx::caps().hdr)
				extract_bloom->setBilinearFilter(0, true); // 2x2 average into the half-res target
			extract_bloom->setRenderSource(buffer);
			extract_bloom->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_BLOOM));
			source = TEXTURE_BLOOM;
		}

		u8 downsample_start = 0;
		if (enable_volumetric_light) {
			// The volumetric pass is expensive (a raymarch per target pixel)
			// and its output only reaches the screen through the blur of the
			// bloom chain, whose first target is half resolution anyway. So
			// with performance_tradeoffs enabled, render it directly into the
			// first downsample target and skip the then-redundant first
			// bloom_downsample step.
			const bool half_res = g_settings->getFlag("performance_tradeoffs");

			shader_id = client->getShaderSource()->getShaderRaw("volumetric_light");
			auto volume = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { source, TEXTURE_DEPTH });
			volume->setRenderSource(buffer);
			if (half_res) {
				// Only the color input is minified; depth stays unfiltered
				// because the shader does exact comparisons against 1.0.
				volume->setBilinearFilter(0, true);
				volume->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_SCALE_DOWN));
				source = TEXTURE_SCALE_DOWN;
				downsample_start = 1;
			} else {
				buffer->setTexture(TEXTURE_VOLUME, scale, "volume", bloom_format);
				volume->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_VOLUME));
				source = TEXTURE_VOLUME;
			}
		}

		// downsample
		shader_id = client->getShaderSource()->getShaderRaw("bloom_downsample");
		for (u8 i = downsample_start; i < MIPMAP_LEVELS; i++) {
			auto step = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { source });
			step->setRenderSource(buffer);
			step->setBilinearFilter(0, true);
			step->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_SCALE_DOWN + i));
			source = TEXTURE_SCALE_DOWN + i;
		}
	}

	// Bloom pt 2
	if (enable_bloom) {
		// upsample
		shader_id = client->getShaderSource()->getShaderRaw("bloom_upsample");
		for (u8 i = MIPMAP_LEVELS - 1; i > 0; i--) {
			auto step = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { u8(TEXTURE_SCALE_DOWN + i - 1), source });
			step->setRenderSource(buffer);
			step->setBilinearFilter(0, true);
			step->setBilinearFilter(1, true);
			step->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, u8(TEXTURE_SCALE_UP + i - 1)));
			source = TEXTURE_SCALE_UP + i - 1;
		}
	}

	// Dynamic Exposure pt2
	if (enable_auto_exposure) {
		shader_id = client->getShaderSource()->getShaderRaw("update_exposure");
		auto update_exposure = pipeline->addStep<PostProcessingStep>(shader_id, std::vector<u8> { TEXTURE_EXPOSURE_1, u8(TEXTURE_SCALE_DOWN + MIPMAP_LEVELS - 1) });
		update_exposure->setBilinearFilter(1, true);
		update_exposure->setRenderSource(buffer);
		update_exposure->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_EXPOSURE_2));
	}

	// FXAA
	u8 final_stage_source = scene_color;

	if (enable_fxaa) {
		final_stage_source = TEXTURE_FXAA;

		buffer->setTexture(TEXTURE_FXAA, scale, "fxaa", color_format);
		shader_id = client->getShaderSource()->getShaderRaw("fxaa");
		PostProcessingStep *effect = pipeline->createOwned<PostProcessingStep>(shader_id, std::vector<u8> { scene_color });
		pipeline->addStep(effect);
		effect->setBilinearFilter(0, true);
		effect->setRenderSource(buffer);
		effect->setRenderTarget(pipeline->createOwned<TextureBufferOutput>(buffer, TEXTURE_FXAA));
	}

	// final merge
	shader_id = client->getShaderSource()->getShaderRaw("second_stage");
	std::vector<u8> final_inputs { final_stage_source, TEXTURE_SCALE_UP, TEXTURE_EXPOSURE_2,
			TEXTURE_DEPTH /* fx: underwater caustics and shafts */ };
	// fixed slots: texture4 AO, texture5 volumetrics, texture6 bounce light
	// (the depth map stands in for a pass that is off: never sampled then)
	if (fx::caps().enabled) {
		final_inputs.push_back(fx_ao && !fx_taa ? TEXTURE_FX_AO : TEXTURE_DEPTH);
		final_inputs.push_back(fx_vol ? TEXTURE_FX_VOLUMETRIC : TEXTURE_DEPTH);
		final_inputs.push_back(fx_gi ? TEXTURE_FX_GI : TEXTURE_DEPTH);
		final_inputs.push_back(fx_clouds ? TEXTURE_FX_CLOUDS : TEXTURE_DEPTH);
	}
	PostProcessingStep *effect = pipeline->createOwned<PostProcessingStep>(shader_id, final_inputs);
	pipeline->addStep(effect);
	if (fx_vol)
		effect->setBilinearFilter(5, true);
	if (fx_gi)
		effect->setBilinearFilter(6, true);
	if (fx_clouds)
		effect->setBilinearFilter(7, true);
	if (enable_ssaa)
		effect->setBilinearFilter(0, true);
	effect->setBilinearFilter(1, true);
	effect->setRenderSource(buffer);

	if (enable_auto_exposure) {
		pipeline->addStep<SwapTexturesStep>(buffer, TEXTURE_EXPOSURE_1, TEXTURE_EXPOSURE_2);
	}

	return effect;
}

void ResolveMSAAStep::run(PipelineContext &context)
{
	context.device->getVideoDriver()->blitRenderTarget(msaa_fbo->getIrrRenderTarget(context),
			target_fbo->getIrrRenderTarget(context));
}
