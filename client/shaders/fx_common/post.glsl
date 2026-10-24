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
// half resolution, with view distance packed in B/A
#define fxAoTex texture4
uniform sampler2D fxAoTex;

// Bilateral 3x3 gather around this pixel in the half-res AO buffer: the
// denoise and the depth-aware upsample in one (no separate blur pass).
// Neighbours count by how close their view distance is to this pixel's,
// so AO does not halo across depth edges. texelFetch: no filter needed.
vec2 fxSampleAO(vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	if (d >= 0.99999)
		return vec2(1.0);
	float dist = fxLinearDepth(d) / fxNearFar.y;
	ivec2 hs = textureSize(fxAoTex, 0);
	ivec2 c = ivec2(uv * vec2(hs));
	vec2 acc = vec2(0.0);
	float wsum = 0.0;
	for (int j = -1; j <= 1; j++)
	for (int i = -1; i <= 1; i++) {
		vec4 s = texelFetch(fxAoTex, clamp(c + ivec2(i, j), ivec2(0), hs - 1), 0);
		float sd = (s.b * 255.0 + s.a) / 255.0;
		float rel = abs(sd - dist) / max(dist, 1e-5);
		float w = exp(-rel * rel * 400.0) * (i == 0 && j == 0 ? 2.0 : 1.0) + 1e-5;
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

#ifdef FX_VOLUMETRICS
// wave 3: in-scattered light (rgb, linear) and transmittance (a), half res
#define fxVolTex texture5
uniform sampler2D fxVolTex;
// Height fog, analytic: density sigma(h) = k * exp(-max(h - base, 0) / H)
// integrated along the view ray in closed form (no marching). Then the
// light glowing in the air from the volumetric pass (half res) is added.
vec3 fxApplyVolumetrics(vec3 color, vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	vec3 P = fxScenePos(uv, d >= 0.99999 ? 0.9999 : d);
	vec3 E = fxEyePos;
	float len = min(length(P - E), fxNearFar.y);
	if (fxHeightFog > 0.0 && len > 0.0) {
		const float H = 12.0 * FX_BS;                         // scale height
		float base = fxHeightFogBase * FX_BS - fxCameraOffsetY; // scene space
		float y0 = E.y - base, y1 = P.y - base;
		// optical depth along the ray: density exp(-y/H) above base, full
		// density below it (a shaft or a cave under the fog line stays
		// foggy at every view angle)
		float dy = y1 - y0;
		float lo = min(y0, y1), hi = max(y0, y1);
		float integral = max(min(hi, 0.0) - lo, 0.0)
			+ H * (exp(-max(lo, 0.0) / H) - exp(-max(hi, 0.0) / H));
		float od = abs(dy) > 1e-3 * FX_BS ? integral / abs(dy) : exp(-max(y0, 0.0) / H);
		od *= len * fxHeightFog * 0.0004;
		float T = exp(-od);
		float day = clamp(fxSunStrength * 1.2, 0.08, 1.0);
		color = color * T + fxToLinear(fxFogColor) * day * (1.0 - T);
	}
	return color + texture2D(fxVolTex, uv).rgb;
}
#endif

#ifdef FX_CLOUDS
// volumetric clouds (fx_clouds pass, half res): light scattered towards the
// eye (rgb) and transmittance (a); the pass stopped at the scene, so terrain
// in front of a cloud stays clear
#define fxCloudTex texture7
uniform sampler2D fxCloudTex;
vec3 fxApplyClouds(vec3 color, vec2 uv, float expo)
{
	vec4 c = texture2D(fxCloudTex, uv);
	return color * c.a + c.rgb * expo;
}
#endif

#ifdef FX_GI
// wave 3: bounce light arriving at each pixel (linear), half res and noisy:
// four bilinear taps around the pixel smooth it
#define fxGiTex texture6
uniform sampler2D fxGiTex;
vec3 fxApplyGI(vec3 color, vec2 uv)
{
	if (fxGi <= 0.0)
		return color;
	vec2 px = 1.0 / vec2(textureSize(fxGiTex, 0));
	vec3 gi = (texture2D(fxGiTex, uv + px * vec2(-1.2, -0.4)).rgb
		+ texture2D(fxGiTex, uv + px * vec2(1.2, 0.4)).rgb
		+ texture2D(fxGiTex, uv + px * vec2(0.4, -1.2)).rgb
		+ texture2D(fxGiTex, uv + px * vec2(-0.4, 1.2)).rgb) * 0.25;
	// what the surface reflects: its own colour, normalised (an albedo proxy)
	vec3 albedo = color / (max(max(color.r, color.g), color.b) + 0.08);
	return color + albedo * gi * fxGi * 0.15;
}
#endif

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
	float warm = clamp((b.r - max(b.g * 0.85, b.b)) / max(b.r, 1e-3) * 2.0, 0.0, 1.0);
	float mask = smoothstep(0.15, 0.9, fxLuma(b)) * warm;
	if (mask <= 0.001)
		return uv;
	// rising shimmer: two layers of value noise, squashed horizontally and
	// stretched vertically (fx_heat_haze_stretch), scrolling upwards; sized
	// against the screen height so it looks the same at any resolution
	float t = fxTime;
	vec2 p = vec2(uv.x / (texel.x / texel.y), uv.y) * (60.0 / fxHeatHazeScale);
	p.y /= fxHeatHazeStretch;
	p.y += t * 1.6;
	vec2 off = vec2(fxVNoise2(p) - 0.5, fxVNoise2(p * 1.9 + vec2(7.3, t * 0.9)) - 0.5);
	off.x += (fxVNoise2(p * 2.7 + vec2(3.1, -t * 1.3)) - 0.5) * 0.6;
	// mostly sideways wobble, a little vertical; ~5 px at 1080p, in screen
	// height units so it scales with the resolution
	return uv + off * vec2(texel.x / texel.y, 0.45) * (5.0 / 1080.0) * mask * fxHeatHaze;
}
#endif

/*
	World and camera effects (wave 1): fog blur, the End sky, portals,
	lightning, lens dirt, grade, vignette, damage. Situational strength comes
	from the moods (nether.fx_fog_blur = ... in client/fx_moods/).
*/

// Swirl while standing in a portal: the view twists around the centre and
// breathes, more the longer you stand in it.
vec2 fxDropletUV(vec2 uv, out float mask);

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

// Distance from the eye (0..1 of the far plane) at uv; 1 for the sky.
// Radial, not planar: planar depth shrinks towards the screen edges, so
// anything keyed on it would fade as the view turns away.
float fxViewDist01(vec2 uv)
{
	float d = texture2D(fxDepth, uv).r;
	return d >= 0.99999 ? 1.0 : min(length(fxScenePos(uv, d) - fxEyePos) / fxNearFar.y, 0.99999);
}

// Distant things soften into the fog. A gather blur whose radius grows with
// distance; samples nearer than this pixel don't bleed in (no halos around
// near objects). Display-encoded in and out, before the HDR chain. Pixels
// in the near 20% of the view do no work at all.
vec3 fxApplyFogBlur(vec3 col, vec2 uv)
{
	if (fxFogBlur <= 0.0)
		return col;
	float dist = fxViewDist01(uv);
	if (dist >= 1.0)
		return col; // the sky (and stars, nebula, aurora) stays sharp
	float f = smoothstep(0.2, 0.9, dist);
	float radius = f * fxFogBlur * 0.0035; // of the screen height
	if (radius < 0.0004)
		return col;
	vec2 px = vec2(radius * fxDepthAspect(), radius);
	vec3 acc = col;
	float wsum = 1.0;
	for (int i = 0; i < 8; i++) {
		float t = (float(i) + 0.5) / 8.0;
		float ang = float(i) * 2.39996; // golden angle
		vec2 o = vec2(cos(ang), sin(ang)) * sqrt(t) * px;
		float w = smoothstep(dist * 0.7, dist * 0.9, fxViewDist01(uv + o));
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

// Wave 4 sky: physically based colour (single-scatter Rayleigh + Mie,
// analytic air mass) blended over the game's sky by day in the Overworld,
// and an aurora on cold nights. Sky pixels only (clouds write depth).
vec3 fxAtmosphere(vec3 dir, vec3 sun)
{
	const vec3 BR = vec3(0.045, 0.105, 0.25);  // Rayleigh, relative per channel
	const float BM = 0.021;                     // Mie
	float m = 1.0 / (max(dir.y, 0.0) + 0.1);    // view air mass
	float ms = 1.0 / (max(sun.y, 0.0) + 0.1);   // sun air mass
	float mu = dot(dir, sun);
	vec3 Ts = exp(-(BR + BM) * ms * 1.2);       // sunlight reaching the air: reddens low
	float pr = 0.75 * (1.0 + mu * mu);
	const float g = 0.76;
	float pm = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5) * 0.08;
	vec3 ray = (1.0 - exp(-BR * m * 1.2)) * pr;
	vec3 mie = vec3(1.0 - exp(-BM * m * 1.2)) * pm;
	return (ray + mie) * Ts * 4.0 * smoothstep(-0.08, 0.05, sun.y);
}

vec3 fxAuroraColor(vec3 dir)
{
	if (dir.y <= 0.02)
		return vec3(0.0);
	// curtains: project onto a sky plane, wavy bands drifting slowly
	vec2 q = dir.xz / (dir.y + 0.15) * 1.4;
	float t = fxTime * 0.03;
	float band = 0.0;
	for (int i = 0; i < 3; i++) {
		float fi = float(i);
		float w = q.x * (0.6 + fi * 0.25) + fxFbm3(vec3(q * 0.5, t + fi)) * 2.5 + t * 2.0 + fi * 1.7;
		band += exp(-pow(sin(w) * 2.2, 2.0)) * (0.6 - fi * 0.15);
	}
	float rays = 0.6 + 0.4 * fxNoise3(vec3(q.x * 9.0, q.y * 0.6, t * 3.0));
	float fade = smoothstep(0.02, 0.25, dir.y) * (1.0 - 0.6 * smoothstep(0.6, 1.0, dir.y));
	vec3 col = mix(vec3(0.15, 1.0, 0.45), vec3(0.6, 0.25, 1.0), 0.6 * smoothstep(0.3, 0.95, dir.y));
	return col * band * rays * fade * 0.35;
}

vec3 fxApplySky(vec3 color, vec2 uv)
{
	if (fxOverworld < 0.01 || texture2D(fxDepth, uv).r < 0.99999)
		return color;
	vec3 dir = fxSafeNormalize(fxScenePos(uv, 1.0) - fxEyePos);
	float k = fxSkyPbr * fxOverworld * fxIsDay * (1.0 - fxStorm * 0.7) * (1.0 - fxWet * 0.5);
	if (k > 0.001)
		color = mix(color, fxAtmosphere(dir, fxSunDir), k);
	float night = (1.0 - fxIsDay) + fxIsDay * (1.0 - smoothstep(-0.05, 0.1, fxSunDir.y));
	float a = fxAurora * fxCold * fxOverworld * clamp(night, 0.0, 1.0) * (1.0 - fxWet);
	if (a > 0.001)
		color += fxAuroraColor(dir) * a;
	return color;
}

// Water running down the lens after surfacing: drops at fixed spots, each
// sliding down and fading; refracts the image a little where they are.
vec2 fxDropletUV(vec2 uv, out float mask)
{
	mask = 0.0;
	float k = fxDroplets * fxDropletsFx;
	if (k <= 0.001)
		return uv;
	vec2 p = uv * vec2(1.0 / fxDepthAspect(), 1.0) * 9.0;
	vec2 cell = floor(p);
	float h = fxHash13(vec3(cell, 1.0));
	vec2 c = vec2(fxHash13(vec3(cell, 2.0)), fxHash13(vec3(cell, 3.0))) * 0.7 + 0.15;
	c.y -= (1.0 - fxDroplets) * (0.4 + h * 0.6); // slides down as it dries
	vec2 d = fract(p) - c;
	float r = 0.12 + h * 0.12;
	float drop = smoothstep(r, r * 0.6, length(d * vec2(1.0, 0.8))) * step(0.45, h);
	mask = drop * k;
	return uv - d / 9.0 * 0.6 * mask;
}

// Smudges that light up where bright light (the bloom buffer, gathered
// wide) crosses them. Linear. 2D value noise, 3 octaves, only where there
// is bright light around.
float fxNoise2(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = fxHash13(vec3(i, 0.0)), b = fxHash13(vec3(i + vec2(1.0, 0.0), 0.0));
	float c = fxHash13(vec3(i + vec2(0.0, 1.0), 0.0)), d = fxHash13(vec3(i + vec2(1.0, 1.0), 0.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 fxLensDirtColor(vec2 uv)
{
#ifdef ENABLE_BLOOM
	if (fxLensDirt <= 0.0)
		return vec3(0.0);
	vec3 light = (texture2D(bloom, clamp(uv + vec2(-0.12, -0.12), 0.0, 1.0)).rgb
		+ texture2D(bloom, clamp(uv + vec2(0.12, -0.12), 0.0, 1.0)).rgb
		+ texture2D(bloom, clamp(uv + vec2(-0.12, 0.12), 0.0, 1.0)).rgb
		+ texture2D(bloom, clamp(uv + vec2(0.12, 0.12), 0.0, 1.0)).rgb) * 0.25;
	if (dot(light, vec3(1.0)) < 0.02)
		return vec3(0.0); // nothing bright around: skip the noise
	vec2 p = uv * vec2(1.0 / fxDepthAspect(), 1.0) * 5.0;
	float n = fxNoise2(p) * 0.55 + fxNoise2(p * 2.7 + 7.0) * 0.3 + fxNoise2(p * 7.1 + 3.0) * 0.15;
	float dirt = smoothstep(0.5, 0.8, n);
	return light * dirt * fxLensDirt * 0.8;
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
