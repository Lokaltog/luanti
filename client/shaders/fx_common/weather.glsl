// fx: weather on surfaces (wave 2). Included by nodes_shader/opengl_fragment.glsl.
// Rain wets what is open to the sky (darker, glossy, puddles with ripples on
// top faces); snow settles on top faces. Sky exposure comes from the vertex
// light (fxSkyExp, set in the vertex shader). Linear light.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#if defined(FX_ENABLE) && !defined(FX_SURFACE_ACTIVE)
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

vec3 fxWeatherSurface(vec3 lin, vec3 albedo, vec3 N)
{
	float sky = smoothstep(0.35, 0.75, fxSkyExp);
	if (sky <= 0.0)
		return lin;
	float up = smoothstep(0.6, 0.95, N.y);
	vec3 wp = (worldPosition + cameraOffset) / FX_BS;

	// snow on top faces, broken up at the edges of its cover
	float snow = fxSnowCover * fxSnow * sky * up;
	if (snow > 0.001) {
		float m = smoothstep(0.35, 0.65, fxFbm3(vec3(wp.xz * 0.8, 0.0)) + snow * 0.6 - 0.3);
		snow = clamp(snow * m * 1.4, 0.0, 1.0);
		float light = fxLuma(lin) / max(fxLuma(albedo), 0.04);
		lin = mix(lin, vec3(0.92, 0.95, 1.0) * light, snow);
	}

	float wet = fxWet * fxWetness * sky * (1.0 - snow);
	if (wet <= 0.001)
		return lin;
	float w = clamp(wet, 0.0, 1.0);
	lin *= mix(1.0, 0.62, w * 0.8); // wet materials darken
	float puddle = up * w * smoothstep(0.55, 0.7, fxFbm3(vec3(wp.xz * 0.35, 3.0)) + (wet - 1.0) * 0.2);
	vec3 n = N;
	if (puddle > 0.0) {
		vec2 r = fxRipples(wp.xz) * fxRaining;
		n = fxSafeNormalize(N + vec3(r.x, 0.0, r.y) * puddle);
		lin = mix(lin, lin * 0.45, puddle * 0.6); // standing water over the ground
	}
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = fxSafeNormalize(eye - worldPosition);
	float cosv = clamp(dot(n, V), 0.0, 1.0);
	float fres = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
	float gloss = mix(0.25, 1.0, puddle);
	float daylight = clamp(1.0 - nightRatio, 0.1, 1.0);
	vec3 sky_c = fxToLinear(fogColor.rgb) * daylight;
	lin += sky_c * fres * gloss * w * 0.8;
	vec3 H = fxSafeNormalize(fxSunDir + V);
	float spec = pow(max(dot(n, H), 0.0), mix(60.0, 400.0, puddle)) * fxSunStrength * gloss * w * 3.0;
	lin += fxToLinear(dayLight) * spec * sky;
	return lin;
}
#endif
