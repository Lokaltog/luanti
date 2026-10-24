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

float fxDepthAspect()
{
	vec2 s = vec2(textureSize(fxDepth, 0));
	return s.y / max(s.x, 1.0); // uv x per uv y, for round kernels
}

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

/*
	World and camera effects (wave 1): fog blur, the End sky, portals,
	lightning, lens dirt, grade, vignette, damage. Situational strength comes
	from the moods (nether.fx_fog_blur = ... in client/fx_moods/).
*/

// Swirl while standing in a portal: the view twists around the centre and
// breathes, more the longer you stand in it.
vec2 fxPortalUV(vec2 uv)
{
	float p = fxPortal * fxPortalWarp;
	if (p <= 0.001)
		return uv;
	vec2 c = uv - 0.5;
	float r = length(c);
	float a = p * (1.0 - smoothstep(0.0, 0.8, r)) * (1.6 + 0.6 * sin(fxTime * 0.9));
	float s = sin(a), co = cos(a);
	c = mat2(co, s, -s, co) * c;
	c *= 1.0 - 0.05 * p * sin(fxTime * 2.3 + r * 14.0);
	return c + 0.5;
}

// View distance (0..1 of the far plane) at uv; 1 for the sky
float fxViewDist01(vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	if (d >= 0.99999)
		return 1.0;
	return length(fxScenePos(uv, d) - fxEyePos) / fxNearFar.y;
}

// Distant things soften into the fog. A gather blur whose radius grows with
// distance; samples nearer than this pixel don't bleed in (no halos around
// near objects). Display-encoded in and out, before the HDR chain.
vec3 fxApplyFogBlur(vec3 col, vec2 uv)
{
	if (fxFogBlur <= 0.0)
		return col;
	float dist = fxViewDist01(uv);
	float f = smoothstep(0.2, 0.9, dist);
	float radius = f * fxFogBlur * 0.0035; // of the screen height
	if (radius < 0.0004)
		return col;
	vec2 px = vec2(radius * fxDepthAspect(), radius);
	vec3 acc = col;
	float wsum = 1.0;
	for (int i = 0; i < 12; i++) {
		float t = (float(i) + 0.5) / 12.0;
		float ang = float(i) * 2.39996; // golden angle
		vec2 o = vec2(cos(ang), sin(ang)) * sqrt(t) * px;
		float sd = fxViewDist01(uv + o);
		float w = smoothstep(dist * 0.7, dist * 0.9, sd);
		acc += texture2D(rendered, uv + o).rgb * w;
		wsum += w;
	}
	return acc / wsum;
}

// Stars and a slow nebula over the End, on sky pixels. Linear.
vec3 fxEndSkyColor(vec2 uv)
{
	float k = fxEnd * fxEndSky;
	if (k <= 0.001 || texture2D(fxDepth, uv).r < 0.99999)
		return vec3(0.0);
	vec3 dir = fxSafeNormalize(fxScenePos(uv, 1.0) - fxEyePos);
	vec3 q = dir * 3.0 + vec3(0.0, fxTime * 0.004, fxTime * 0.002);
	float n = fxFbm3(q);
	float wisps = smoothstep(0.45, 0.85, n) * smoothstep(0.3, 0.7, fxFbm3(q * 2.2 + 4.0));
	vec3 nebula = mix(vec3(0.30, 0.08, 0.45), vec3(0.05, 0.30, 0.42), fxFbm3(q * 0.7 + 9.0)) * wisps * 0.6;
	vec3 cell = floor(dir * 220.0);
	float h = fxHash13(cell);
	float star = step(0.9965, h) * (0.5 + 0.5 * sin(fxTime * (1.0 + h * 3.0) + h * 40.0));
	return (nebula + vec3(0.9, 0.85, 1.0) * star * 1.5) * k;
}

// Smudges that light up where bright light (the bloom buffer, gathered wide)
// crosses them. Linear.
vec3 fxLensDirtColor(vec2 uv)
{
#ifdef ENABLE_BLOOM
	if (fxLensDirt <= 0.0)
		return vec3(0.0);
	vec3 light = vec3(0.0);
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++)
		light += texture2D(bloom, clamp(uv + vec2(float(x), float(y)) * 0.12, 0.0, 1.0)).rgb;
	light /= 9.0;
	if (dot(light, vec3(1.0)) < 0.02)
		return vec3(0.0); // nothing bright around: skip the noise
	vec3 p = vec3(uv * vec2(1.0 / fxDepthAspect(), 1.0) * 5.0, 0.0);
	float dirt = smoothstep(0.55, 0.85, fxFbm3(p)) * 0.8 + smoothstep(0.7, 0.95, fxFbm3(p * 3.0 + 7.0)) * 0.6;
	return light * dirt * fxLensDirt * 0.6;
#else
	return vec3(0.0);
#endif
}

// Lightning: the whole scene lights up, slightly blue. Linear, before
// exposure is final.
vec3 fxApplyFlash(vec3 col)
{
	float f = fxFlash * fxLightningFlash;
	return col * (1.0 + f * 2.5) + vec3(0.80, 0.86, 1.0) * f * 0.12;
}

// After the tonemap (display-encoded): warmth, vignette, portal tint,
// damage flash and the low-health heartbeat.
vec3 fxFinalGrade(vec3 col, vec2 uv)
{
	float w = fxGradeWarmth;
	col *= vec3(1.0 + 0.10 * w, 1.0 + 0.02 * w, 1.0 - 0.12 * w);
	vec2 c = (uv - 0.5) * vec2(1.15, 1.0);
	float edge = smoothstep(0.30, 0.95, length(c));
	col *= 1.0 - edge * fxVignette * 0.7;
	// portal: violet cast creeping in from the edges
	float p = fxPortal * fxPortalWarp;
	col = mix(col, col * vec3(0.8, 0.55, 1.15) + vec3(0.10, 0.0, 0.16) * (0.5 + 0.5 * sin(fxTime * 3.0)),
		clamp(p * (0.25 + 0.5 * edge), 0.0, 1.0));
	// damage and low health
	float dmg = fxDamage * fxDamageFx;
	float beat = pow(max(sin(fxTime * 6.0), 0.0), 8.0) + 0.6 * pow(max(sin(fxTime * 6.0 - 0.9), 0.0), 8.0);
	float low = fxLowHealth * fxDamageFx;
	float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
	col = mix(col, vec3(luma), clamp(low * 0.45, 0.0, 1.0));
	float red = clamp(edge * (dmg * 0.7 + low * (0.25 + 0.35 * beat)), 0.0, 0.9);
	col = mix(col, vec3(0.55, 0.02, 0.02), red);
	return col;
}

#endif // FX_HDR_COMPOSITE
