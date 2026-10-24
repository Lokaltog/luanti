#define rendered texture0
#define bloom texture1

#ifdef GL_ES
// Dithering requires sufficient floating-point precision
#ifndef GL_FRAGMENT_PRECISION_HIGH
#undef ENABLE_DITHERING
#endif
#endif

struct ExposureParams {
	float compensationFactor;
};

uniform sampler2D rendered;
uniform sampler2D bloom;

uniform vec2 texelSize0;

uniform ExposureParams exposureParams;
uniform lowp float bloomIntensity;
uniform lowp float saturation;

#ifdef FX_HDR_COMPOSITE
uniform float fxBloomIntensity;
uniform float fxExposure;
uniform float fxGradeSaturation;
uniform float fxGradeContrast;
// Mood from the afx fork's audio: x = tension 0..1, y = cozy 0..1, already
// scaled by fx_mood_grade (zero when afx is off).
uniform vec2 fxMood;

// Display-referred mood grade. Dread drains colour, crushes the darks a
// little, cools the shadows and closes in at the edges; home warms up.
vec3 fxMoodGrade(vec3 c, vec2 uv)
{
	float t = fxMood.x, h = fxMood.y;
	float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
	c = mix(c, vec3(luma), 0.45 * t);
	c = mix(c, c * c * (3.0 - 2.0 * c), 0.3 * t);     // a little more contrast
	vec3 cool = vec3(0.86, 0.95, 1.10);
	c *= mix(vec3(1.0), cool, t * (1.0 - luma));       // cool the shadows
	vec2 d = uv - 0.5;
	c *= 1.0 - 0.55 * t * smoothstep(0.25, 0.75, dot(d, d) * 2.2);
	vec3 warm = vec3(1.07, 1.0, 0.88);
	c *= mix(vec3(1.0), warm, 0.6 * h);
	c = mix(c, vec3(luma) + 1.12 * (c - vec3(luma)), h); // a touch richer
	return clamp(c, 0.0, 1.0);
}

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

CENTROID_ VARYING_ mediump vec2 varTexCoord;

#ifdef ENABLE_AUTO_EXPOSURE
VARYING_ float exposure; // linear exposure factor, see vertex shader
#endif

#ifdef ENABLE_BLOOM

vec4 applyBloom(vec4 color, vec2 uv)
{
	vec3 light = texture2D(bloom, uv).rgb;
#ifdef ENABLE_BLOOM_DEBUG
	if (uv.x > 0.5 && uv.y < 0.5)
		return vec4(light, color.a);
	if (uv.x < 0.5)
		return color;
#endif
	color.rgb = mix(color.rgb, light, bloomIntensity);
	return color;
}

#endif

#if ENABLE_TONE_MAPPING

/* Hable's UC2 Tone mapping parameters
	A = 0.22;
	B = 0.30;
	C = 0.10;
	D = 0.20;
	E = 0.01;
	F = 0.30;
	W = 11.2;
	equation used:  ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F
*/

// highp for GLES, see <https://github.com/luanti-org/luanti/pull/14688>
highp vec3 uncharted2Tonemap(highp vec3 x)
{
	return ((x * (0.22 * x + 0.03) + 0.002) / (x * (0.22 * x + 0.3) + 0.06)) - 0.03333;
}

vec4 applyToneMapping(vec4 color)
{
	color = vec4(pow(color.rgb, vec3(2.2)), color.a);
	const float gamma = 1.6;
	const float exposureBias = 5.5;
	color.rgb = uncharted2Tonemap(exposureBias * color.rgb);
	// Precalculated white_scale from
	//vec3 whiteScale = 1.0 / uncharted2Tonemap(vec3(W));
	vec3 whiteScale = vec3(1.036015346);
	color.rgb *= whiteScale;
	return vec4(pow(color.rgb, vec3(1.0 / gamma)), color.a);
}
#endif

vec3 applySaturation(vec3 color, float factor)
{
	// Calculate the perceived luminosity from the RGB color.
	// See also: https://www.w3.org/WAI/GL/wiki/Relative_luminance
	float brightness = dot(color, vec3(0.2125, 0.7154, 0.0721));
	return mix(vec3(brightness), color, factor);
}

#ifdef ENABLE_DITHERING
// From http://alex.vlachos.com/graphics/Alex_Vlachos_Advanced_VR_Rendering_GDC2015.pdf
// and https://www.shadertoy.com/view/MslGR8 (5th one starting from the bottom)
// NOTE: `frag_coord` is in pixels (i.e. not normalized UV).
vec3 screen_space_dither(highp vec2 frag_coord) {
	// Iestyn's RGB dither (7 asm instructions) from Portal 2 X360, slightly modified for VR.
	highp vec3 dither = vec3(dot(vec2(171.0, 231.0), frag_coord));
	dither.rgb = fract(dither.rgb / vec3(103.0, 71.0, 97.0));

	// Subtract 0.5 to avoid slightly brightening the whole viewport.
	return (dither.rgb - 0.5) / 255.0;
}
#endif

void main(void)
{
	vec2 uv = varTexCoord.st;
#ifdef ENABLE_SSAA
	vec4 color = vec4(0.);
	for (float dx = 1.; dx < SSAA_SCALE; dx += 2.)
	for (float dy = 1.; dy < SSAA_SCALE; dy += 2.)
		color += texture2D(rendered, uv + texelSize0 * vec2(dx, dy)).rgba;
	color /= SSAA_SCALE * SSAA_SCALE / 4.;
#else
	vec4 color = texture2D(rendered, uv).rgba;
#endif

	// translate to linear colorspace (approximate)
	color.rgb = pow(color.rgb, vec3(2.2));

#ifdef FX_HDR_COMPOSITE
	// fx: HDR path. Bloom is added (energy above threshold, see extract_bloom),
	// tonemapping happens BEFORE any clamp, so highlights roll off instead of clipping.
	color.rgb *= exposureParams.compensationFactor * fxExposure;
#ifdef ENABLE_AUTO_EXPOSURE
	color.rgb *= exposure;
#endif
#ifdef FX_SURFACES
	if (fxUnderwater > 0.5)
		color.rgb = fxApplyUnderwater(color.rgb, uv);
#endif
#ifdef ENABLE_BLOOM
	vec3 fxBloom = texture2D(bloom, uv).rgb;
	color.rgb += fxBloom * fxBloomIntensity;
#endif

#if defined(FX_DEBUG_VIEW) && FX_DEBUG_VIEW == 3
	gl_FragColor = vec4(fxFalseColor(dot(color.rgb, vec3(0.2126, 0.7152, 0.0722))), 1.0);
	return;
#elif defined(FX_DEBUG_VIEW) && FX_DEBUG_VIEW == 4 && defined(ENABLE_BLOOM)
	gl_FragColor = vec4(pow(fxBloom, vec3(1.0 / 2.2)), 1.0);
	return;
#endif

#if FX_TONEMAPPER == 1
	color.rgb = fxAgx(color.rgb);
#elif FX_TONEMAPPER == 2
	color.rgb = pow(fxAces(color.rgb), vec3(1.0 / 2.2));
#else
	color.rgb = pow(clamp(color.rgb, 0.0, 1.0), vec3(1.0 / 2.2));
#if ENABLE_TONE_MAPPING
	color = applyToneMapping(color);
#endif
#endif
	color.rgb = applySaturation(color.rgb, saturation);
	color.rgb = fxMoodGrade(color.rgb, uv);
#ifdef ENABLE_DITHERING
	color.rgb += screen_space_dither(gl_FragCoord.xy);
#endif
	gl_FragColor = vec4(color.rgb, 1.0);
	return;
#endif

#ifdef ENABLE_BLOOM_DEBUG
	if (uv.x > 0.5 || uv.y > 0.5)
#endif
	{
		color.rgb *= exposureParams.compensationFactor;
#ifdef ENABLE_AUTO_EXPOSURE
		color.rgb *= exposure;
#endif
	}

#ifdef ENABLE_BLOOM
	color = applyBloom(color, uv);
#endif


	color.rgb = clamp(color.rgb, vec3(0.), vec3(1.));

	// return to sRGB colorspace (approximate)
	color.rgb = pow(color.rgb, vec3(1.0 / 2.2));

#ifdef ENABLE_BLOOM_DEBUG
	if (uv.x > 0.5 || uv.y > 0.5)
#endif
	{
#if ENABLE_TONE_MAPPING
		color = applyToneMapping(color);
#endif

		color.rgb = applySaturation(color.rgb, saturation);
	}

#ifdef ENABLE_DITHERING
	// Apply dithering just before quantisation
	color.rgb += screen_space_dither(gl_FragCoord.xy);
#endif

	gl_FragColor = vec4(color.rgb, 1.0); // force full alpha to avoid holes in the image.
}
