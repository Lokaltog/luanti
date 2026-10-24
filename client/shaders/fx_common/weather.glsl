// fx: weather on surfaces (wave 2). Included by nodes_shader/opengl_fragment.glsl.
// Rain wets what is open to the sky (darker, glossy, puddles with ripples on
// top faces); snow settles on top faces. Sky exposure comes from the vertex
// light (fxSkyExp, set in the vertex shader). Linear light.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
// compiled in only when one of its effects is on (fx_use_wet, _snow,
// _translucency): a smaller shader keeps more pixels in flight
#if defined(FX_ENABLE) && !defined(FX_SURFACE_ACTIVE) && \
		(defined(FX_ON_USE_WET) || defined(FX_ON_USE_SNOW) || defined(FX_ON_USE_TRANSLUCENCY))
#define FX_WEATHER_ACTIVE 1
VARYING_ float fxSkyExp; // sky light reaching the vertex, 0..1

// Rain rings on puddles: per 0.5-node cell a drop lands at a random spot
// and phase; the ring's slope is the normal offset (xz).
vec2 fxRipples(vec2 p)
{
	vec2 acc = vec2(0.0);
	for (int k = 0; k < 2; k++) {
		vec2 q = p * 2.0 + float(k) * vec2(0.37, 0.71);
		vec2 cell = floor(q);
		vec2 f = fract(q);
		float h = fxHash13(vec3(cell, float(k) * 7.0));
		vec2 c = vec2(fxHash13(vec3(cell, 3.0 + float(k))), fxHash13(vec3(cell, 5.0 + float(k)))) * 0.6 + 0.2;
		float t = fract(fxTime * 0.9 + h);
		vec2 d = f - c;
		float r = length(d);
		float ring = exp(-pow((r - t * 0.45) * 24.0, 2.0)) * (1.0 - t);
		acc += d / max(r, 1e-3) * ring;
	}
	return acc * 0.35;
}

// Wave 4: sunlight shining through thin things (leaves, plants; and texels
// an _s map marks as subsurface: porosity/SSS channel 65..255). Seen
// against the sun, they glow in their own colour.
vec3 fxTranslucent(vec3 lin, vec3 albedo, float sss)
{
	if (fxTranslucency <= 0.0 || sss <= 0.0 || fxIsDay < 0.5)
		return lin;
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = fxSafeNormalize(worldPosition - eye); // eye -> surface
	float t = max(dot(V, fxSunDir), 0.0);
	t = t * t * t * t;
	float sky = smoothstep(0.2, 0.7, fxSkyExp);
	return lin + albedo * fxToLinear(dayLight) * t * sss * sky * fxSunStrength * fxTranslucency * 1.5;
}

// Rain and snow on a surface open to the sky. With material maps (mat:
// true) the LabPBR channels drive it: porosity (_s blue 0..64) says how
// much the material soaks up (darkens, stays matte, dries slowly) versus
// carries a film of water (glossy, dries first); the height map (_n alpha)
// says where water collects, so joints and cracks flood before faces.
// sm: _s texel, nm: _n texel, Nm: the mapped normal.
vec3 fxWeatherSurface(vec3 lin, vec3 albedo, vec3 N, bool mat, vec4 sm, vec4 nm, vec3 Nm)
{
	float sky = smoothstep(0.35, 0.75, fxSkyExp);
	if (sky <= 0.0)
		return lin;
	float up = smoothstep(0.6, 0.95, N.y);
	vec3 wp = (worldPosition + cameraOffset) / FX_BS;

	// snow on top faces, broken up at the edges of its cover
	float snow = fxSnowCover * fxSnow * sky * up;
	if (snow > 0.001) {
		float m = smoothstep(0.35, 0.65, fxFbm2(wp.xz * 0.8) + snow * 0.6 - 0.3);
		// in a height-mapped tile it lies in the dips first
		if (mat && nm.r + nm.g > 0.002)
			m = clamp(m + (0.6 - nm.a) * 0.8 * snow, 0.0, 1.0);
		snow = clamp(snow * m * 1.4, 0.0, 1.0);
		float light = fxLuma(lin) / max(fxLuma(albedo), 0.04);
		lin = mix(lin, vec3(0.92, 0.95, 1.0) * light, snow);
	}

	float wetness = fxWet * fxWetness * sky * (1.0 - snow);
	if (wetness <= 0.001)
		return lin;
	// porosity: 0 dense (glazed, polished, metal) .. 1 soaks it up (wool,
	// dirt); subsurface texels (leaves) count as half; no maps: in between
	float por = 0.5, f0 = 0.04;
	bool metal = false;
	if (mat) {
		float b = sm.b * 255.0;
		por = b <= 64.5 ? b / 64.0 : 0.5;
		metal = sm.g * 255.0 >= 229.5;
		f0 = metal ? 1.0 : sm.g;
	}
	// porous things stay dark longer as it dries; dense ones lose the film first
	float w_dark = smoothstep(0.0, 0.5 + 0.5 * por, wetness);
	float w_film = smoothstep(0.15 + 0.4 * por, 0.55 + 0.4 * por, wetness);
	if (!metal)
		lin *= mix(1.0, mix(0.8, 0.45, por), w_dark); // soaked materials darken
	// puddles: on up faces, where the ground is low; with a height map, in
	// its joints and cracks first, as the level rises with the wetness
	float puddle = 0.0;
	float big = fxFbm2(wp.xz * 0.35 + 31.0);
	if (mat && nm.r + nm.g > 0.002) {
		float level = 1.0 - 0.35 * w_film + (big - 0.5) * 0.3;
		puddle = up * smoothstep(level, level - 0.06, nm.a) * w_film;
	} else {
		puddle = up * w_film * smoothstep(0.55, 0.7, big + (wetness - 1.0) * 0.2);
	}
	vec3 n = mat && dot(Nm, Nm) > 0.5 ? normalize(mix(Nm, N, w_film * 0.5)) : N; // the film smooths the relief
	if (puddle > 0.0) {
		vec2 r = fxRipples(wp.xz) * fxRaining;
		n = fxSafeNormalize(mix(n, N, puddle) + vec3(r.x, 0.0, r.y) * puddle);
		lin = mix(lin, lin * 0.55, puddle * 0.6); // standing water over the ground
	}
	// the film: a smooth layer of water over the material (F0 0.02)
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = fxSafeNormalize(eye - worldPosition);
	float cosv = clamp(dot(n, V), 0.0, 1.0);
	float fres = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
	float gloss = max(mix(0.85, 0.3, por) * w_film, puddle);
	float daylight = clamp(1.0 - nightRatio, 0.1, 1.0);
	vec3 sky_c = fxToLinear(fogColor.rgb) * daylight;
	lin += sky_c * fres * gloss * 0.8;
	vec3 H = fxSafeNormalize(fxSunDir + V);
	float spec = pow(max(dot(n, H), 0.0), mix(40.0, 500.0, gloss)) * fxSunStrength * gloss * 3.0;
	lin += fxToLinear(dayLight) * spec * sky;
	return lin;
}
#endif
