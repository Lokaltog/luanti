// Luanti "fx" fork: shader defines and generated includes
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Defines come from the frozen caps plus the SHADERS-apply params; uniforms
// come from three uniform blocks uploaded once per frame (FxParams, FxFrame,
// FxLights; see renderer.h and fx_common/uniforms.glsl). There is no
// per-material uniform setter.

#include "renderer.h"
#include <cctype>
#include "fx_test.h"

#include "client/shader.h"
#include "light_volume.h"

namespace fx
{

namespace
{

class FxConstantSetter : public IShaderConstantSetter
{
public:
	void onGenerate(const std::string &name, ShaderConstants &constants) override
	{
		Renderer *r = renderer();
		if (!r || !r->caps.enabled)
			return;
		const Caps &c = r->caps;
		const Params &p = r->last_shader_params;
		constants["FX_ENABLE"] = 1;
		constants["FX_LIGHT_MAX"] = MAX_LIGHTS;
		constants["FX_VOLUME_SX"] = LightVolume::SX;
		constants["FX_VOLUME_SY"] = LightVolume::SY;
		constants["FX_VOLUME_SZ"] = LightVolume::SZ;
		constants["FX_VOLUME_COLS"] = LightVolume::ATLAS_COLS;
		if (c.colored_light)
			constants["FX_COLORED_LIGHT"] = 1;
		if (c.light_volume)
			constants["FX_LIGHT_VOLUME"] = 1;
		if (c.hdr)
			constants["FX_HDR_COMPOSITE"] = 1;
		if (c.materials)
			constants["FX_MATERIALS"] = 1;
		if (c.taa)
			constants["FX_TAA"] = 1;
		if (c.ao)
			constants["FX_AO"] = 1;
		if (c.volumetrics)
			constants["FX_VOLUMETRICS"] = 1;
		if (c.gi)
			constants["FX_GI"] = 1;
		if (c.surfaces) {
			constants["FX_SURFACES"] = 1;
			if (p.ssr)
				constants["FX_SSR"] = 1;
		}
		// hard toggles: FX_ON_<NAME> for every enabled SHADERS checkbox
		for (const auto &i : paramInfo()) {
			if (i.type != ParamType::Bool || i.apply != Apply::SHADERS || !i.tune ||
					getFloat(p, i) < 0.5f)
				continue;
			std::string def = "FX_ON_";
			for (const char *s = i.name; *s; s++)
				def += (char)std::toupper((unsigned char)*s);
			constants[def] = 1;
		}
		constants["FX_TONEMAPPER"] = p.tonemapper;
		constants["FX_DEBUG_VIEW"] = p.debug_view;
		if (test::active())
			constants["FX_NAN_CHECK"] = 1; // paints NaN/Inf pixels pure magenta
	}
};

} // namespace

void registerShaderSetters(Renderer &r)
{
	r.shsrc->addShaderConstantSetter(std::make_unique<FxConstantSetter>());
}

std::string generatedShaderInclude(const std::string &path)
{
	if (path == "fx_generated/params.glsl")
		return paramBlockGlsl();
	return "";
}

} // namespace fx
