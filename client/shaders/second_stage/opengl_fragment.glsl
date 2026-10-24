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

#include "fx_common/post.glsl"

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
#if defined(FX_ENABLE) && defined(FX_HDR_COMPOSITE)
	uv = fxPortalUV(uv);
#endif
#if defined(FX_ENABLE) && defined(FX_HDR_COMPOSITE) && defined(ENABLE_BLOOM)
	uv = fxHeatHazeUV(uv, texelSize0);
#endif
#ifdef ENABLE_SSAA
	vec4 color = vec4(0.);
	for (float dx = 1.; dx < SSAA_SCALE; dx += 2.)
	for (float dy = 1.; dy < SSAA_SCALE; dy += 2.)
		color += texture2D(rendered, uv + texelSize0 * vec2(dx, dy)).rgba;
	color /= SSAA_SCALE * SSAA_SCALE / 4.;
#else
	vec4 color = texture2D(rendered, uv).rgba;
#endif

#ifdef FX_ENABLE
#ifdef FX_TAA
	// fx: restore detail TAA softens (cross-shaped unsharp mask)
	if (fxSharpen > 0.0) {
		vec3 n = texture2D(rendered, uv + vec2(0.0, texelSize0.y)).rgb
			+ texture2D(rendered, uv - vec2(0.0, texelSize0.y)).rgb
			+ texture2D(rendered, uv + vec2(texelSize0.x, 0.0)).rgb
			+ texture2D(rendered, uv - vec2(texelSize0.x, 0.0)).rgb;
		color.rgb = max(color.rgb + (color.rgb * 4.0 - n) * fxSharpen * 0.25, vec3(0.0));
	}
#endif
#endif

#if defined(FX_ENABLE) && defined(FX_HDR_COMPOSITE) && !defined(ENABLE_SSAA)
	color.rgb = fxApplyFogBlur(color.rgb, uv);
#endif
	// translate to linear colorspace (approximate)
	color.rgb = pow(color.rgb, vec3(2.2));

#ifdef FX_HDR_COMPOSITE
	/*
		fx: HDR composite, linear: AO -> exposure -> underwater -> bloom ->
		tonemap -> grade -> dither. Bloom (extract_bloom) is the energy above a
		threshold, so it is added, and the tonemapper rolls highlights off
		instead of clipping them.
	*/
#if defined(FX_AO) && !defined(FX_TAA)
	color.rgb = fxApplyAO(color.rgb, uv);
#if defined(FX_DEBUG_VIEW) && FX_DEBUG_VIEW == 7
	gl_FragColor = vec4(vec3(fxSampleAO(uv).r), 1.0);
	return;
#endif
#endif
#ifdef FX_GI
	color.rgb = fxApplyGI(color.rgb, uv);
#endif
#ifdef FX_VOLUMETRICS
	color.rgb = fxApplyVolumetrics(color.rgb, uv);
#endif
	color.rgb *= exposureParams.compensationFactor * fxExposure;
#ifdef ENABLE_AUTO_EXPOSURE
	color.rgb *= exposure;
#endif
	color.rgb += fxEndSkyColor(uv);
	color.rgb = fxApplyFlash(color.rgb);
	vec3 fxTrans = vec3(1.0);
#ifdef FX_SURFACES
	color.rgb = fxApplyUnderwater(color.rgb, uv, fxTrans);
#endif
#ifdef FX_NAN_CHECK
	// test mode: NaN/Inf from the scene or the bloom chain become countable
	bool fxBad = any(isnan(color.rgb)) || any(isinf(color.rgb));
#endif
#ifdef ENABLE_BLOOM
	// bloom was extracted before the water: it is absorbed like the scene
	vec3 fxBloom = texture2D(bloom, uv).rgb * fxTrans;
#ifdef FX_NAN_CHECK
	fxBad = fxBad || any(isnan(fxBloom)) || any(isinf(fxBloom));
#endif
	color.rgb += fxBloom * fxBloomIntensity;
#endif
	color.rgb += fxLensDirtColor(uv) * fxTrans;
#ifdef FX_NAN_CHECK
	if (fxBad) {
		gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
		return;
	}
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
	color.rgb = fxFinalGrade(color.rgb, varTexCoord.st);
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
