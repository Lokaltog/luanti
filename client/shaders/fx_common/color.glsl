// fx: shared helpers (colour space, fog, depth, noise, caustics).
//
// Colour contract (R2): every fx shading term is computed in LINEAR light.
// The 3D scene buffer (RGBA16F when fx runs) stores display-encoded values,
// x^(1/2.2), exactly like upstream, so sky, clouds, particles and every other
// upstream shader keep working unchanged. Shaders decode with fxToLinear when
// they read the buffer and encode with fxToDisplay when they write it; the
// composite (second_stage) decodes once and the tonemapper encodes once.
// Values above 1.0 survive (float buffer) and are what blooms.
#include "fx_common/uniforms.glsl"
#ifdef FX_ENABLE
#ifndef FX_COLOR_GLSL
#define FX_COLOR_GLSL

vec3 fxToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 fxToDisplay(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
float fxLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); } // Rec. 709, linear

// normalize() of a zero vector is NaN; meshes without normals exist
vec3 fxSafeNormalize(vec3 v)
{
	float l = dot(v, v);
	return l > 1e-12 ? v * inversesqrt(l) : vec3(0.0, 1.0, 0.0);
}

// Upstream's fog colour (tinted toward the fog's own hue at high clarity),
// decoded to linear: fog is blended in linear light.
vec3 fxFogLinear(vec3 fog_color, float clarity)
{
	float m = max(max(fog_color.r, fog_color.g), fog_color.b);
	if (m < 1e-7)
		m = 1.0;
	return fxToLinear(fog_color * pow(fog_color / m, vec3(2.0 * clarity)));
}

// Interleaved gradient noise (Jimenez 2014), per pixel
float fxIGN(vec2 frag)
{
	return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

// Depth buffer value -> view distance (scene units)
float fxLinearDepth(float d)
{
	float n = fxNearFar.x, f = fxNearFar.y;
	return 2.0 * n * f / (f + n - (d * 2.0 - 1.0) * (f - n));
}

// Screen uv + depth buffer value -> scene-space position (relative to cameraOffset)
vec3 fxScenePos(vec2 uv, float depth)
{
	vec4 p = fxInvViewProj * vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
	return p.xyz / p.w;
}

// Sum-of-warps caustics (after joltz0r's "water turbulence"), a web of lines.
// Division-safe: the original divides by sin/cos, which reach 0.
float fxCaustic(vec2 p, float t, float sharpness)
{
	vec2 i = p;
	float c = 1.0;
	const float inten = 0.005;
	for (int n = 0; n < 4; n++) {
		float tt = t * (1.0 - (3.5 / float(n + 1)));
		i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
		vec2 s = vec2(sin(i.x + tt), cos(i.y + tt));
		s = vec2(abs(s.x) < 1e-4 ? 1e-4 : s.x, abs(s.y) < 1e-4 ? 1e-4 : s.y);
		c += 1.0 / max(length(p * inten / s), 1e-4);
	}
	c /= 4.0;
	c = 1.17 - pow(c, 1.4);
	return clamp(pow(abs(c), sharpness), 0.0, 1.0);
}

// Value noise and fBm (hash: Dave Hoskins)
float fxHash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float fxNoise3(vec3 p)
{
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(fxHash13(i), fxHash13(i + vec3(1, 0, 0)), f.x),
	               mix(fxHash13(i + vec3(0, 1, 0)), fxHash13(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(fxHash13(i + vec3(0, 0, 1)), fxHash13(i + vec3(1, 0, 1)), f.x),
	               mix(fxHash13(i + vec3(0, 1, 1)), fxHash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float fxFbm3(vec3 p)
{
	float v = 0.0, a = 0.5;
	for (int i = 0; i < 5; i++) {
		v += a * fxNoise3(p);
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		a *= 0.5;
	}
	return v;
}

// Project along the sun so caustic patterns fall the way the light does
vec2 fxSunUV(vec3 p)
{
	return (p.xz - fxSunDir.xz / max(fxSunDir.y, 0.2) * p.y) / FX_BS * 0.9;
}

#endif
#endif
