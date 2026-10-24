// fx: composite helpers (AgX/ACES, false colour, underwater, AO, heat haze).
// Included by second_stage/opengl_fragment.glsl. Linear light throughout;
// the scene buffer is decoded once before these run (see color.glsl).
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#ifdef FX_ENABLE
// full-resolution depth (texture3), always an input of the final pass
#define fxDepth texture3
uniform sampler2D fxDepth;

#if defined(FX_AO) && !defined(FX_TAA)
// without TAA, AO (R) and contact shadows (G) are applied here (texture4):
// half resolution, blurred, with view distance packed in B/A
#define fxAoTex texture4
uniform sampler2D fxAoTex;

// Depth-aware upsample: of the four nearest half-res texels, the ones at
// this pixel's distance count, so AO does not halo across depth edges.
vec2 fxSampleAO(vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	if (d >= 0.99999)
		return vec2(1.0);
	float dist = length(fxScenePos(uv, d) - fxEyePos) / fxNearFar.y;
	vec2 hs = vec2(textureSize(fxAoTex, 0));
	vec2 st = uv * hs - 0.5;
	vec2 f = fract(st);
	vec2 b = (floor(st) + 0.5) / hs;
	vec2 acc = vec2(0.0);
	float wsum = 0.0;
	for (int j = 0; j <= 1; j++)
	for (int i = 0; i <= 1; i++) {
		vec4 s = texture2D(fxAoTex, b + vec2(float(i), float(j)) / hs);
		float sd = (s.b * 255.0 + s.a) / 255.0;
		float rel = abs(sd - dist) / max(dist, 1e-5);
		float bil = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
		float w = bil * exp(-rel * rel * 400.0) + 1e-5;
		acc += s.rg * w;
		wsum += w;
	}
	return acc / wsum;
}

// AO darkens what reads as ambient/indirect light; bright pixels (lit by the
// sun, lamps, emissive) keep their light. Contact shadows only touch pixels
// in the mid range, where sun light dominates. Linear.
vec3 fxApplyAO(vec3 color, vec2 uv)
{
	vec2 aocs = fxSampleAO(uv);
	float lum = fxLuma(color);
	color *= mix(aocs.r, 1.0, smoothstep(0.6, 3.0, lum));
	color *= mix(1.0, aocs.g, smoothstep(0.02, 0.15, lum) * (1.0 - smoothstep(1.5, 4.0, lum)));
	return color;
}
#endif
#endif

#ifdef FX_HDR_COMPOSITE

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
	// AgX "look": power + saturation in AgX log space
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
// Returns the colour; `trans` gets the absorption, so bloom can share it.

vec3 fxApplyUnderwater(vec3 color, vec2 uv, out vec3 trans)
{
	trans = vec3(1.0);
	if (fxUnderwater < 0.5)
		return color;
	vec3 cam = fxScenePos(uv, 0.0);  // this pixel's point on the near plane
	// half-submerged camera: the part of the screen above the surface is air
	if (cam.y > fxWaterLevel)
		return color;
	float d = texture2D(fxDepth, uv).r;
	bool sky = d >= 0.99999;
	vec3 dir = fxSafeNormalize(fxScenePos(uv, 0.5) - cam);
	float dist = sky ? 64.0 * FX_BS : length(fxScenePos(uv, d) - cam);
	// only the stretch of the ray that is under water absorbs; past the
	// surface (seen from below) the surface shader takes over
	float wet = dir.y > 1e-4 ? min(dist, (fxWaterLevel - cam.y) / dir.y) : dist;
	vec3 pos = cam + dir * dist;
	float light = fxSunStrength;

	if (!sky && pos.y < fxWaterLevel && fxCaustics > 0.0) {
		float c = fxCaustic(fxSunUV(pos), fxTime * 0.8, 8.0);
		color *= 1.0 + c * fxCaustics * light * 4.0 * exp(-wet / FX_BS * 0.08);
	}

	// shafts: march the view ray through a soft, low-frequency caustic pattern
	float shafts = 0.0;
	if (fxUnderwaterShafts > 0.0 && light > 0.0) {
		float len = min(wet, 24.0 * FX_BS);
		const int STEPS = 24;
		float jitter = fxIGN(gl_FragCoord.xy); // fine noise instead of banding
		for (int i = 0; i < STEPS; i++) {
			float t = (float(i) + jitter) / float(STEPS) * len;
			shafts += fxCaustic(fxSunUV(cam + dir * t) * 0.3, fxTime * 0.5, 5.0) * exp(-t / FX_BS * 0.1);
		}
		shafts = shafts / float(STEPS) * len / FX_BS * 0.3;
	}
	const vec3 water_light = vec3(0.35, 0.75, 0.9);

	// absorption toward a blue-green scatter colour
	trans = exp(-wet / FX_BS * fxWaterAbsorption * vec3(1.6, 0.6, 0.35));
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

#ifdef ENABLE_BLOOM
// Heat haze: shimmer above warm, strongly blooming sources (lava, fire). The
// bloom buffer is the mask, sampled a little below the pixel so the shimmer
// rises above the source instead of sitting on it.
vec2 fxHeatHazeUV(vec2 uv, vec2 texel)
{
	if (fxHeatHaze <= 0.0)
		return uv;
	vec3 b = texture2D(bloom, uv - vec2(0.0, 0.035)).rgb
		+ texture2D(bloom, uv - vec2(0.0, 0.08)).rgb * 0.6;
	// warm sources only: lava and fire are red-heavy, the sun and white lamps are not
	float warm = clamp((b.r - b.b) / max(b.r, 1e-3) * 1.5, 0.0, 1.0);
	float mask = smoothstep(0.15, 0.9, fxLuma(b)) * warm;
	if (mask <= 0.001)
		return uv;
	float t = fxTime;
	vec2 p = uv / texel;
	vec2 off = vec2(sin(p.y * 0.11 + t * 5.0 + sin(p.x * 0.05 + t)),
		cos(p.x * 0.09 - t * 4.0 + sin(p.y * 0.07)));
	return uv + off * texel * 2.5 * mask * fxHeatHaze;
}
#endif

#endif // FX_HDR_COMPOSITE
