// fx M5: material maps (PACK_FORMAT v2 item 5, LabPBR layout).
// Included by nodes_shader/opengl_fragment.glsl. Array-texture tiles only.
//   _n: R,G normal (tangent space), B AO (255 = none), A height (255 = top)
//   _s: R perceptual smoothness, G F0 (>= 230 = metal), B porosity, A emission (255 = none)
// Layer 2i of texture7 is _n, 2i+1 is _s, for base layer i. Bunches without
// authored maps bind a shared 2-layer neutral array; their clamped layer
// index reads an all-zero "normal", which is treated as "no normal map".
#if defined(FX_ENABLE) && defined(FX_MATERIALS) && defined(USE_ARRAY_TEXTURE) && !defined(FX_SURFACE_ACTIVE)
#define FX_MAT_ACTIVE 1
#define fxMatTex texture7
uniform mediump sampler2DArray fxMatTex;
uniform vec3 fxSunDir;
uniform float fxSunStrength;
uniform float fxNormalStrength;
uniform float fxParallaxDepth;
uniform float fxSpecular;
uniform float fxGlints;

const float FX_MAT_BS = 10.0;

mat3 fxTBN;
bool fxTBNok = false;
vec3 fxMatEmission = vec3(0.0);

// Cotangent frame from screen-space derivatives (Schueler 2013): works for
// rotated and world-aligned tiles without tangents in the mesh.
void fxBuildTBN(vec2 uv)
{
	vec3 dp1 = dFdx(worldPosition), dp2 = dFdy(worldPosition);
	vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
	vec3 N = normalize(vNormal);
	vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float d = max(dot(T, T), dot(B, B));
	fxTBNok = d > 1e-20 && dot(vNormal, vNormal) > 0.0;
	float inv = inversesqrt(max(d, 1e-20));
	fxTBN = mat3(T * inv, B * inv, N);
}

vec4 fxMatN(vec2 uv)
{
	return texture(fxMatTex, vec3(uv, float(varTexLayer) * 2.0));
}

vec4 fxMatS(vec2 uv)
{
	return texture(fxMatTex, vec3(uv, float(varTexLayer) * 2.0 + 1.0));
}

bool fxHasNormal(vec4 n)
{
	return n.r + n.g > 0.002;
}

// Texel-stepped parallax occlusion: the height map is sampled with nearest
// filtering, so 16-32 px pixel art shifts by whole texels instead of smearing.
vec2 fxParallax(vec2 uv)
{
	if (fxParallaxDepth <= 0.0 || !fxTBNok)
		return uv;
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = worldPosition - eye;
	float dist = length(V);
	if (dist > 16.0 * FX_MAT_BS)
		return uv;
	vec3 v = normalize(V) * fxTBN; // into tangent space (transpose)
	if (v.z > -0.08)
		return uv; // grazing or from behind: skip
	if (!fxHasNormal(fxMatN(uv)))
		return uv;
	vec2 dir = v.xy / -v.z * fxParallaxDepth;
	const int STEPS = 12;
	const float layer = 1.0 / float(STEPS);
	float cur = 1.0;
	vec2 p = uv;
	float h = fxMatN(p).a;
	for (int i = 0; i < STEPS; i++) {
		if (h >= cur)
			break;
		cur -= layer;
		p += dir * layer;
		h = fxMatN(p).a;
	}
	return mix(p, uv, smoothstep(10.0 * FX_MAT_BS, 16.0 * FX_MAT_BS, dist));
}

// Normal-mapped relief, AO, GGX sun specular, glints, emission.
// Relief is relative to the flat face, so average brightness stays put.
vec3 fxShadeMaterial(vec3 col, vec4 base, vec2 uv)
{
	vec4 nm = fxMatN(uv);
	vec4 sm = fxMatS(uv);
	vec3 N = normalize(vNormal);
	vec3 Nm = N;
	float ao = 1.0;
	if (fxTBNok && fxHasNormal(nm)) {
		vec2 xy = (nm.rg * 2.0 - 1.0) * fxNormalStrength;
		vec3 nts = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
		Nm = normalize(fxTBN * nts);
		ao = mix(1.0, nm.b, clamp(fxNormalStrength, 0.0, 1.0));
	}
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = normalize(eye - worldPosition);
	float day = clamp((1.0 - nightRatio) * fxSunStrength, 0.0, 1.0);
	vec3 L = fxSunDir;

	float rs = (0.55 + 0.45 * max(dot(Nm, L), 0.0)) / (0.55 + 0.45 * max(dot(N, L), 0.0));
	// artificial light: no direction is known per pixel, so light from the
	// viewer's side (what a held torch does) reads best
	float ra = (0.6 + 0.4 * max(dot(Nm, V), 0.0)) / (0.6 + 0.4 * max(dot(N, V), 0.0));
	col *= mix(ra, rs, day) * ao;

	float smoothness = sm.r;
	if (smoothness > 0.0) {
		float rough = max((1.0 - smoothness) * (1.0 - smoothness), 0.045);
		bool metal = sm.g * 255.0 >= 229.5;
		vec3 F0 = metal ? base.rgb : vec3(sm.g);
		vec3 H = normalize(L + V);
		float NdH = max(dot(Nm, H), 0.0), NdL = max(dot(Nm, L), 0.0), NdV = max(dot(Nm, V), 1e-3);
		float a2 = rough * rough;
		float dd = NdH * NdH * (a2 - 1.0) + 1.0;
		float D = a2 / (3.14159 * dd * dd);
		vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
		float k = rough * 0.5;
		float G = NdL / (NdL * (1.0 - k) + k) * NdV / (NdV * (1.0 - k) + k);
		vec3 spec = D * F * G / (4.0 * NdV * max(NdL, 1e-3)) * NdL;
		// only where the sun actually reaches (vertex light as a proxy for shadow)
		float lit = clamp(dot(varColor.rgb, vec3(0.333)) * 2.0, 0.0, 1.0);
		// bounded: this is still added before the HDR encode, and an unbounded
		// GGX peak overflows the fp16 bloom chain (see the review plan)
		col += min(spec * dayLight * day * lit * fxSpecular, vec3(4.0));

		// glints: smooth or metal texels sparkle as the view direction changes
		if (fxGlints > 0.0 && (metal || smoothness > 0.6)) {
			vec2 tsz = vec2(textureSize(fxMatTex, 0).xy);
			vec2 texel = floor(uv * tsz);
			vec3 R = reflect(-V, Nm);
			float seed = dot(floor(R * 6.0), vec3(1.0, 7.0, 13.0));
			float hsh = fract(sin(dot(vec3(texel, seed), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
			float g = smoothstep(0.985, 1.0, hsh) * smoothness;
			col += g * fxGlints * 3.0 * (varColor.rgb + dayLight * day * 0.5) * (metal ? base.rgb : vec3(1.0));
		}
	}
	if (sm.a < 0.999)
		fxMatEmission = base.rgb * sm.a * 255.0 / 254.0;
	return col;
}
#endif
