// fx: composite helpers (AgX/ACES, false colour, underwater, AO fallback).
// Included by second_stage/opengl_fragment.glsl.
#ifdef FX_ENABLE
uniform float fxSharpen;
#if defined(FX_AO) && !defined(FX_TAA)
// without TAA, AO is applied here (texture4)
#define fxAoTex texture4
uniform sampler2D fxAoTex;
#endif
#endif

#ifdef FX_HDR_COMPOSITE
uniform float fxBloomIntensity;
uniform float fxExposure;
uniform float fxGradeSaturation;
uniform float fxGradeContrast;

// AgX, minimal fit by Benjamin Wrensch (MIT), sRGB primaries.
// Input: linear scene-referred. Output: display-encoded (gamma) in [0,1].
vec3 fxAgxContrast(vec3 x)
{
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
		- 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 fxAgx(vec3 v)
{
	const mat3 inset = mat3(
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3(
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float min_ev = -12.47393;
	const float max_ev = 4.026069;
	v = inset * v;
	v = clamp(log2(max(v, vec3(1e-10))), min_ev, max_ev);
	v = (v - min_ev) / (max_ev - min_ev);
	v = fxAgxContrast(v);
	// AgX "look": power + saturation in AgX log space (the "punchy" look is
	// power 1.35, saturation 1.4). Keeps colored lights colored.
	float luma = dot(v, vec3(0.2126, 0.7152, 0.0722));
	v = pow(max(v, vec3(0.0)), vec3(fxGradeContrast));
	luma = pow(max(luma, 0.0), fxGradeContrast);
	v = luma + fxGradeSaturation * (v - luma);
	return clamp(outset * v, 0.0, 1.0);
}

// ACES filmic fit (Narkowicz 2015). Linear in, linear out.
vec3 fxAces(vec3 x)
{
	return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

#ifdef FX_SURFACES
// Underwater: absorption, caustics on everything below, and light shafts
// that poke down from the surface. Needs the depth buffer (texture3).
#define fxDepth texture3
uniform sampler2D fxDepth;
uniform mat4 fxInvViewProj;
uniform vec2 fxNearFar;
uniform vec3 fxSunDir;
uniform float fxSunStrength;
uniform float fxTime;
uniform float fxUnderwater;
uniform float fxUnderwaterShafts;
uniform float fxCaustics;
uniform float fxWaterAbsorption;

const float FX_BS = 10.0;

float fxCausticPost(vec2 p, float t, float sharpness)
{
	vec2 i = p;
	float c = 1.0;
	const float inten = 0.005;
	for (int n = 0; n < 4; n++) {
		float tt = t * (1.0 - (3.5 / float(n + 1)));
		i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
		c += 1.0 / length(vec2(p.x / (sin(i.x + tt) / inten), p.y / (cos(i.y + tt) / inten)));
	}
	c /= 4.0;
	c = 1.17 - pow(c, 1.4);
	return clamp(pow(abs(c), sharpness), 0.0, 1.0);
}

vec3 fxScenePos(vec2 uv, float depth)
{
	vec4 p = fxInvViewProj * vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
	return p.xyz / p.w;
}

vec2 fxSunUV(vec3 p)
{
	// project along the sun so the pattern falls the way the light does
	return (p.xz - fxSunDir.xz / max(fxSunDir.y, 0.2) * p.y) / FX_BS * 0.9;
}

vec3 fxApplyUnderwater(vec3 color, vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	bool sky = d >= 0.99999;
	vec3 cam = fxScenePos(uv, 0.0);  // near plane point, close enough to the eye
	vec3 dir = normalize(fxScenePos(uv, 0.5) - cam);
	// sky pixels: treat as far water so they fade into the scatter colour
	float dist = sky ? 64.0 * FX_BS : length(fxScenePos(uv, d) - cam);
	vec3 pos = cam + dir * dist;
	float light = fxSunStrength;

	// caustics on what we look at (not on the sky)
	if (!sky) {
		float c = fxCausticPost(fxSunUV(pos), fxTime * 0.8, 8.0);
		color *= 1.0 + c * fxCaustics * light * 4.0 * exp(-dist / FX_BS * 0.08);
	}

	// shafts: march the view ray, accumulate light through the caustic pattern
	float len = min(dist, 24.0 * FX_BS);
	float shafts = 0.0;
	const int STEPS = 24;
	// jitter the start per pixel to trade banding for fine noise
	float jitter = fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453);
	for (int i = 0; i < STEPS; i++) {
		float t = (float(i) + jitter) / float(STEPS) * len;
		vec3 p = cam + dir * t;
		// low-frequency, soft pattern: broad beams, not a sharp web
		shafts += fxCausticPost(fxSunUV(p) * 0.3, fxTime * 0.5, 5.0) * exp(-t / FX_BS * 0.1);
	}
	shafts = shafts / float(STEPS) * len / FX_BS * 0.3;
	vec3 water_light = vec3(0.35, 0.75, 0.9);

	// absorption toward a blue-green scatter colour
	vec3 trans = exp(-dist / FX_BS * fxWaterAbsorption * vec3(1.6, 0.6, 0.35));
	color = mix(water_light * 0.07 * light, color, trans);
	color += water_light * shafts * fxUnderwaterShafts * light;
	return color;
}
#endif

vec3 fxFalseColor(float lum)
{
	// log2 luminance stops: blue (-4) .. green (0) .. yellow (+2) .. red (+4)
	float s = clamp((log2(max(lum, 1e-5)) + 4.0) / 8.0, 0.0, 1.0);
	return clamp(vec3(4.0 * s - 2.0, 2.0 - abs(4.0 * s - 2.0), 2.0 - 4.0 * s), 0.0, 1.0);
}
#endif

#if defined(FX_ENABLE) && defined(FX_HDR_COMPOSITE)
// Heat haze: shimmer above strongly blooming sources (lava, fire). The bloom
// buffer is the mask, sampled a little below the pixel so the shimmer rises
// above the source instead of sitting on it.
uniform float fxHeatHaze;
#ifndef FX_SURFACES
uniform float fxTime;
#endif

vec2 fxHeatHazeUV(vec2 uv, vec2 texel)
{
	if (fxHeatHaze <= 0.0)
		return uv;
	vec3 b = texture2D(bloom, uv - vec2(0.0, 0.035)).rgb
		+ texture2D(bloom, uv - vec2(0.0, 0.08)).rgb * 0.6;
	// warm sources only: lava and fire are red-heavy, the sun and white lamps are not
	float warm = clamp((b.r - b.b) * 1.5, 0.0, 1.0);
	float mask = smoothstep(0.15, 0.9, dot(b, vec3(0.3, 0.5, 0.2))) * warm;
	if (mask <= 0.001)
		return uv;
	float t = fxTime;
	vec2 p = uv / texel;
	vec2 off = vec2(sin(p.y * 0.11 + t * 5.0 + sin(p.x * 0.05 + t)) ,
		cos(p.x * 0.09 - t * 4.0 + sin(p.y * 0.07)));
	return uv + off * texel * 2.5 * mask * fxHeatHaze;
}
#endif
