// fx: LabPBR material maps. Included by nodes_shader/opengl_fragment.glsl.
// Array-texture tiles only.
//   _n: R,G normal (DirectX, Y down), B AO (255 = none), A height (255 = top)
//   _s: R perceptual smoothness, G F0 (230+ = metal), B porosity, A emission (255 = none)
// Layer 2i of texture7 is _n, 2i+1 is _s, for base layer i. Bunches without
// authored maps bind a shared 2-layer neutral array; their clamped layer
// index reads an all-zero "normal", treated as "no normal map".
//
// Everything here works in linear light (see color.glsl). Texture reads at
// the parallax-shifted uv use the ORIGINAL uv derivatives (textureGrad), so
// mip selection does not jump where neighbouring pixels march different
// numbers of steps.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#if defined(FX_ENABLE) && defined(FX_MATERIALS) && defined(USE_ARRAY_TEXTURE) && !defined(FX_SURFACE_ACTIVE)
#define FX_MAT_ACTIVE 1
#ifndef FX_PARALLAX_STEPS
#define FX_PARALLAX_STEPS 10
#endif
#define fxMatTex texture7
uniform mediump sampler2DArray fxMatTex;

mat3 fxTBN;
bool fxTBNok = false;
vec2 fxUVdx, fxUVdy;            // derivatives of the unshifted uv
vec3 fxMatEmission = vec3(0.0); // linear, from _s.a
vec4 fxMatSm = vec4(0.0);       // the _s texel fxShadeMaterial read
vec4 fxMatNm = vec4(0.0);       // the _n texel (a: height)
vec3 fxMatNormal = vec3(0.0);   // the mapped normal (world), 0 without maps

// Cotangent frame from screen-space derivatives (Schueler 2013): works for
// rotated and world-aligned tiles without tangents in the mesh. Call once at
// the top of main(), in uniform control flow.
void fxBuildTBN(vec2 uv)
{
	fxUVdx = dFdx(uv);
	fxUVdy = dFdy(uv);
	vec3 dp1 = dFdx(worldPosition), dp2 = dFdy(worldPosition);
	vec3 N = fxSafeNormalize(vNormal);
	vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
	vec3 T = dp2perp * fxUVdx.x + dp1perp * fxUVdy.x;
	vec3 B = dp2perp * fxUVdx.y + dp1perp * fxUVdy.y;
	float d = max(dot(T, T), dot(B, B));
	fxTBNok = d > 1e-20 && dot(vNormal, vNormal) > 1e-12;
	float inv = inversesqrt(max(d, 1e-20));
	fxTBN = mat3(T * inv, B * inv, N);
}

vec4 fxMatN(vec2 uv)
{
	return textureGrad(fxMatTex, vec3(uv, float(varTexLayer) * 2.0), fxUVdx, fxUVdy);
}

vec4 fxMatS(vec2 uv)
{
	return textureGrad(fxMatTex, vec3(uv, float(varTexLayer) * 2.0 + 1.0), fxUVdx, fxUVdy);
}

bool fxHasNormal(vec4 n)
{
	return n.r + n.g > 0.002;
}

// Parallax occlusion mapping on the _n height channel. More layers at
// grazing angles, then the hit is placed between the last two layers by
// linear interpolation, so depth reads as a smooth surface instead of as
// stacked slices. Heights come through the material array's own bilinear
// filter (fx_smooth_material_maps), never stepped per texel.
vec2 fxParallax(vec2 uv)
{
	if (fxParallaxDepth <= 0.0 || !fxTBNok)
		return uv;
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = worldPosition - eye;
	float dist = length(V);
	float far_d = fxParallaxDistance * FX_BS;
	if (dist > far_d)
		return uv;
	vec3 v = (V / max(dist, 1e-4)) * fxTBN; // into tangent space (transpose)
	if (v.z > -0.08)
		return uv; // grazing or from behind
	if (!fxHasNormal(fxMatN(uv)))
		return uv;
	// offset limiting: the shift grows with the view angle only up to ~70
	// degrees, so grazing views don't look bottomless or swim
	vec2 dir = v.xy / max(-v.z, 0.35) * fxParallaxDepth;
	// FX_PARALLAX_STEPS layers at grazing angles, ~40% of that looking
	// straight down, half far away (the interpolation hides coarse steps)
	const float NMAX = float(FX_PARALLAX_STEPS);
	float n = max(2.0, mix(NMAX, NMAX * 0.4, clamp(-v.z, 0.0, 1.0)) * mix(1.0, 0.5, smoothstep(far_d * 0.4, far_d, dist)));
	float layer = 1.0 / n;
	float cur = 1.0;
	vec2 p = uv;
	float h = fxMatN(p).a;
	float prev_h = h, prev_cur = cur;
	vec2 prev_p = p;
	for (int i = 0; i < FX_PARALLAX_STEPS; i++) {
		if (float(i) >= n || h >= cur)
			break;
		prev_p = p;
		prev_h = h;
		prev_cur = cur;
		cur -= layer;
		p += dir * layer;
		h = fxMatN(p).a;
	}
	// interpolate between the layer above the surface and the one below
	float after = h - cur;               // >= 0: below the surface
	float before = prev_h - prev_cur;    // < 0: still above it
	float t = clamp(after / max(after - before, 1e-5), 0.0, 1.0);
	p = mix(p, prev_p, t);
	return mix(p, uv, smoothstep(far_d * 0.7, far_d, dist));
}

// Normal-mapped relief, AO, GGX sun specular, glints, _s emission.
// lin: the engine-lit colour, linear. base: the texel (display-encoded).
// Relief is relative to the flat face, so average brightness stays put.
vec3 fxShadeMaterial(vec3 lin, vec4 base, vec2 uv)
{
	vec4 nm = fxMatN(uv);
	vec4 sm = fxMatS(uv);
	fxMatSm = sm;
	fxMatNm = nm;
	vec3 N = fxSafeNormalize(vNormal);
	vec3 Nm = N;
	float ao = 1.0;
	if (fxTBNok && fxHasNormal(nm)) {
		vec2 xy = (nm.rg * 2.0 - 1.0) * fxNormalStrength;
		vec3 nts = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
		Nm = fxSafeNormalize(fxTBN * nts);
		ao = mix(1.0, nm.b, clamp(fxNormalStrength, 0.0, 1.0));
		fxMatNormal = Nm;
	}
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = fxSafeNormalize(eye - worldPosition);
	float day = clamp((1.0 - nightRatio) * fxSunStrength, 0.0, 1.0);
	vec3 L = fxSunDir;

	float rs = (0.55 + 0.45 * max(dot(Nm, L), 0.0)) / (0.55 + 0.45 * max(dot(N, L), 0.0));
	// artificial light: no direction per pixel yet, so light from the
	// viewer's side (what a held torch does) reads best
	float ra = (0.6 + 0.4 * max(dot(Nm, V), 0.0)) / (0.6 + 0.4 * max(dot(N, V), 0.0));
	lin *= mix(ra, rs, day) * ao;

	vec3 albedo = fxToLinear(base.rgb);
	float smoothness = sm.r;
	if (smoothness > 0.0 && day > 0.0) {
		float rough = max((1.0 - smoothness) * (1.0 - smoothness), 0.045);
		bool metal = sm.g * 255.0 >= 229.5;
		vec3 F0 = metal ? albedo : vec3(sm.g);
		vec3 H = fxSafeNormalize(L + V);
		float NdH = max(dot(Nm, H), 0.0), NdL = max(dot(Nm, L), 0.0), NdV = max(dot(Nm, V), 1e-3);
		float a2 = rough * rough;
		float dd = NdH * NdH * (a2 - 1.0) + 1.0;
		float D = a2 / (3.14159 * dd * dd);
		vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
		float k = rough * 0.5;
		float G = NdL / (NdL * (1.0 - k) + k) * NdV / (NdV * (1.0 - k) + k);
		vec3 spec = D * F * G / (4.0 * NdV * max(NdL, 1e-3)) * NdL;
		// only where the sun reaches (vertex sky light as a shadow proxy)
		float lit = clamp(fxLuma(fxToLinear(varColor.rgb)) * 3.0, 0.0, 1.0);
		vec3 sun = fxToLinear(dayLight) * day * lit;
		// bounded: an HDR highlight, not an fp16 overflow
		lin += min(spec * sun * fxSpecular, vec3(16.0));

		// glints: smooth or metal texels sparkle as the half vector moves,
		// fading out once a texel is smaller than a pixel
		if (fxGlints > 0.0 && (metal || smoothness > 0.6)) {
			vec2 tsz = vec2(textureSize(fxMatTex, 0).xy);
			float px = max(length(fxUVdx * tsz), length(fxUVdy * tsz));
			float lod = 1.0 - smoothstep(0.6, 1.5, px);
			if (lod > 0.0) {
				float hsh = fxHash13(vec3(floor(uv * tsz), dot(floor(H * 4.0), vec3(1.0, 7.0, 13.0))));
				float g = smoothstep(0.985, 1.0, hsh) * smoothness * lod;
				lin += g * fxGlints * 4.0 * sun * (metal ? albedo : vec3(1.0));
			}
		}
	}
	if (sm.a < 0.999)
		fxMatEmission = albedo * sm.a * 255.0 / 254.0;
	return lin;
}
#endif
