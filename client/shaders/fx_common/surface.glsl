// fx: glass and water surfaces. Included by nodes_shader/opengl_fragment.glsl.
//
// The opaque scene was copied (colour + depth) at the start of the
// transparent pass; surfaces refract it, tint it and reflect it, in linear
// light. fxSurface returns what the surface itself contributes (reflection,
// lit body/frame) and, separately, the light transmitted from behind: that
// is already fogged for its own distance, so only the surface's own part is
// fogged afterwards (no double fog).
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#include "fx_common/water.glsl"
#ifdef FX_SURFACE_ACTIVE
#define fxSceneColor texture4
#define fxSceneDepth texture5
uniform sampler2D fxSceneColor;
uniform sampler2D fxSceneDepth;

vec2 fxProject(vec3 p, out float depth)
{
	vec4 c = fxViewProj * vec4(p, 1.0);
	if (c.w <= 1e-4) { // behind the camera: the projection would flip
		depth = 2.0;   // callers treat depth > 1 as "off screen"
		return vec2(-1.0);
	}
	vec3 ndc = c.xyz / c.w;
	depth = ndc.z * 0.5 + 0.5;
	return ndc.xy * 0.5 + 0.5;
}

// World-space normal offset for the surface pattern.
vec3 fxPatternNormal(vec3 n, vec3 wp)
{
	vec3 a = abs(n);
	// tangent axes on the face
	vec3 t1 = a.y > 0.5 ? vec3(1.0, 0.0, 0.0) : (a.x > 0.5 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
	vec3 t2 = cross(n, t1);
	vec3 p = wp / FX_BS;
#if FX_SURFACE == 2
	// water: two octaves of moving ripples, plus longer swells that run with
	// the wind, as high as the biome and weather make them (fxWaterChop)
	float t = fxTime;
	vec2 q = p.xz * 1.7;
	vec2 g = vec2(cos(q.x * 2.1 + t * 1.3) + cos((q.x + q.y) * 3.3 - t * 1.7) * 0.5,
	              cos(q.y * 2.3 - t * 1.1) + cos((q.x - q.y) * 2.9 + t * 1.9) * 0.5) * 0.06;
	// (top faces: replaced by the layered waves in fxSurface)
	return t1 * g.x + t2 * g.y;
#elif FX_PATTERN == 1
	// fluted: vertical ridges, 6 per node
	float u = dot(p, t1) * 6.0 * 6.2831853;
	return t1 * sin(u) * 0.35;
#elif FX_PATTERN == 3
	// rippled: static interference rings
	vec2 q = vec2(dot(p, t1), dot(p, t2)) * 6.2831853;
	return (t1 * sin(q.x * 3.0 + sin(q.y * 2.0)) + t2 * sin(q.y * 3.0 + sin(q.x * 2.0))) * 0.12;
#else
	return vec3(0.0);
#endif
}

#if FX_SURFACE == 1
// Rounded bevel along the pane edges that meet a frame (framed glass; the
// mesher puts the mask in the vertex Aux). The surface curves down towards
// the frame, so refraction and reflection bend there.
vec3 fxBevelNormal(vec3 n, vec3 wp)
{
	if (fxGlassBevel <= 0.0 || varFxEdges == 0u)
		return vec3(0.0);
	vec3 a = abs(n);
	// (U, V) = (Y, Z) for X faces, (X, Z) for Y faces, (X, Y) for Z faces
	vec3 U = a.x > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 V = a.z > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
	vec3 local = wp / FX_BS - floor(wp / FX_BS + 0.5); // -0.5 .. 0.5 in the node
	float u = dot(local, U), v = dot(local, V);
	float w = fxGlassBevel / 16.0;
	vec3 d = vec3(0.0);
	// t: 1 at the frame, 0 where the bevel ends; quarter-round profile
	float t;
	if ((varFxEdges & 1u) != 0u) { t = clamp(1.0 - (u + 0.5) / w, 0.0, 1.0); d -= U * t * t; }
	if ((varFxEdges & 2u) != 0u) { t = clamp(1.0 - (0.5 - u) / w, 0.0, 1.0); d += U * t * t; }
	if ((varFxEdges & 4u) != 0u) { t = clamp(1.0 - (v + 0.5) / w, 0.0, 1.0); d -= V * t * t; }
	if ((varFxEdges & 8u) != 0u) { t = clamp(1.0 - (0.5 - v) / w, 0.0, 1.0); d += V * t * t; }
	return d * 0.8;
}
#endif

// Linear colour of the scene copy
vec3 fxSampleScene(vec2 uv)
{
#if FX_PATTERN == 2
	// frosted: small disk blur
	vec2 px = 1.0 / vec2(textureSize(fxSceneColor, 0));
	vec3 acc = vec3(0.0);
	const vec2 taps[8] = vec2[8](vec2(1.0, 0.0), vec2(0.7, 0.7), vec2(0.0, 1.0), vec2(-0.7, 0.7),
		vec2(-1.0, 0.0), vec2(-0.7, -0.7), vec2(0.0, -1.0), vec2(0.7, -0.7));
	for (int i = 0; i < 8; i++)
		acc += fxToLinear(texture2D(fxSceneColor, uv + taps[i] * px * 5.0).rgb);
	return (acc + fxToLinear(texture2D(fxSceneColor, uv).rgb) * 2.0) / 10.0;
#else
	return fxToLinear(texture2D(fxSceneColor, uv).rgb);
#endif
}

#ifdef FX_SSR
// March the reflected ray against the scene depth copy. Returns linear rgb +
// hit weight.
#ifndef FX_SSR_STEPS
#define FX_SSR_STEPS 16
#endif
#ifndef FX_SSR_REFINE
#define FX_SSR_REFINE 3
#endif
vec4 fxTraceReflection(vec3 origin, vec3 dir)
{
	// geometric steps that cover fx_ssr_distance in FX_SSR_STEPS: fine near
	// the surface, coarse far away
	const float G = 1.2;
	float GN = pow(G, float(FX_SSR_STEPS));
	float stepLen = fxSsrDistance * FX_BS * (G - 1.0) / (GN - 1.0);
	vec3 p = origin + dir * (0.15 * FX_BS + stepLen);
	vec3 prev = origin;
	for (int i = 0; i < FX_SSR_STEPS; i++) {
		float rd;
		vec2 uv = fxProject(p, rd);
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || rd > 1.0)
			break;
		float sd = texture2D(fxSceneDepth, uv).r;
		if (rd > sd) {
			float gap = fxLinearDepth(rd) - fxLinearDepth(sd);
			if (gap < max(stepLen * 1.5, 0.5 * FX_BS)) {
				// refine between prev and p
				vec3 a = prev, b = p;
				for (int j = 0; j < FX_SSR_REFINE; j++) {
					vec3 m = (a + b) * 0.5;
					float md;
					vec2 muv = fxProject(m, md);
					if (md > texture2D(fxSceneDepth, muv).r) b = m; else a = m;
				}
				float bd;
				vec2 huv = fxProject(b, bd);
				vec2 edge = min(huv, 1.0 - huv);
				float fade = clamp(min(edge.x, edge.y) * 12.0, 0.0, 1.0)
					* (1.0 - float(i) / float(FX_SSR_STEPS));
				return vec4(fxToLinear(texture2D(fxSceneColor, huv).rgb), fade);
			}
			break; // went behind something thick: no reliable hit
		}
		prev = p;
		p += dir * stepLen;
		stepLen *= G;
	}
	return vec4(0.0);
}
#endif

// The base texture's luminance, bilinear by hand (texelFetch), so the bend
// it adds to refraction is smooth even when the sampler filters nearest
// (pixel art keeps crisp texels; the light bent through it shouldn't step).
float fxSmoothLum(vec2 uv)
{
#ifdef USE_ARRAY_TEXTURE
	ivec2 sz = textureSize(baseTexture, 0).xy;
#else
	ivec2 sz = textureSize(baseTexture, 0);
#endif
	vec2 st = uv * vec2(sz) - 0.5;
	vec2 f = fract(st);
	ivec2 i0 = ivec2(floor(st));
	float l[4];
	for (int k = 0; k < 4; k++) {
		ivec2 p = i0 + ivec2(k & 1, k >> 1);
		p = ((p % sz) + sz) % sz; // tiles repeat
#ifdef USE_ARRAY_TEXTURE
		vec4 t = texelFetch(baseTexture, ivec3(p, int(varTexLayer)), 0);
#else
		vec4 t = texelFetch(baseTexture, p, 0);
#endif
		l[k] = dot(t.rgb, vec3(0.3, 0.6, 0.1)) * (1.0 - t.a * 0.5);
	}
	return mix(mix(l[0], l[1], f.x), mix(l[2], l[3], f.x), f.y);
}

// lit: the surface texture as lit by the engine, linear. base: raw texel.
// Returns the surface's own light; `transmitted` gets what comes through.
vec3 fxSurface(vec3 lit, vec4 base, out vec3 transmitted)
{
	vec2 screen = vec2(textureSize(fxSceneColor, 0));
	vec2 suv = gl_FragCoord.xy / screen;
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = fxSafeNormalize(worldPosition - eye);
	vec3 N = fxSafeNormalize(vNormal);
	bool from_below = dot(N, V) > 0.0;
	if (from_below)
		N = -N;
	vec3 dn = fxPatternNormal(N, worldPosition);
#ifdef FX_WATER_ACTIVE
	// the top of a body of water: layered waves sized to it (water.glsl)
	FxWaterInfo wi;
	float wave_h = 0.0, wave_amp = 0.0;
	bool top = abs(N.y) > 0.5;
	vec2 wabs = (worldPosition.xz + cameraOffset.xz) / FX_BS;
	// flowing liquid tilts its top normal downstream (content_mapblock)
	vec2 wflow = top ? vNormal.xz / max(abs(vNormal.y), 0.2) : vec2(0.0);
	if (top) {
		wi = fxWaterAt(worldPosition / FX_BS);
		float footprint = length(fwidth(wabs));
		vec3 hw = fxWaterWaves(wabs, wi, footprint, wflow, wave_amp);
		wave_h = hw.x;
		dn = vec3(-hw.y, 0.0, -hw.z) * (from_below ? -1.0 : 1.0);
	}
#endif
#if FX_SURFACE == 1
	dn += fxBevelNormal(N, worldPosition + cameraOffset);
#endif

	// The texture's own luminance bends light a little, so streaky glass
	// textures distort without any pattern.
	float lum = fxSmoothLum(varTexCoord.st);
	vec2 tex_bend = vec2(dFdx(lum), dFdy(lum)) * 0.15;

	// Perspective-correct refraction offset: project a point pushed along the
	// perturbed normal and take the screen delta.
#if FX_SURFACE == 2
	float thickness = 0.35 * FX_BS;
#else
	float thickness = 0.12 * FX_BS;
#endif
	float d0, d1;
	vec2 p0 = fxProject(worldPosition, d0);
	vec2 p1 = fxProject(worldPosition - dn * thickness * fxRefraction, d1);
	vec2 ruv = suv + (d1 > 1.0 ? vec2(0.0) : p1 - p0) + tex_bend * fxRefraction;
	// never refract in something that is in front of the surface
	if (texture2D(fxSceneDepth, ruv).r < gl_FragCoord.z)
		ruv = suv;
	vec3 behind = fxSampleScene(ruv);

	// Reflection
	vec3 Np = fxSafeNormalize(N + dn);
	vec3 R = reflect(V, Np);
	float cosv = clamp(dot(-V, Np), 0.0, 1.0);
	float fresnel = 0.04 + 0.96 * pow(1.0 - cosv, 5.0);
	float daylight = clamp(1.0 - nightRatio, 0.15, 1.0);
	vec3 fog_lin = fxToLinear(fogColor.rgb);
	vec3 refl = mix(fog_lin, fog_lin * vec3(0.6, 0.75, 1.3), clamp(R.y, 0.0, 1.0)) * daylight;
#ifdef FX_SSR
	if (!from_below && fresnel * fxReflection > 0.03) {
		vec4 hit = fxTraceReflection(worldPosition, R);
		refl = mix(refl, hit.rgb, hit.a);
	}
#endif

#if FX_SURFACE == 2
	// Water: absorption by depth (red goes first), caustics on what's below
	float below = max(fxLinearDepth(texture2D(fxSceneDepth, ruv).r) - fxLinearDepth(gl_FragCoord.z), 0.0);
	if (from_below)
		below = 0.0; // looking up from inside: the post pass handles absorption
	// absorption: the biome's scatter colour is what survives longest
	vec3 absorb = mix(vec3(1.6, 0.6, 0.35), (1.0 - fxWaterTint) * 1.8 + 0.08, 0.7);
	vec3 trans = exp(-below / FX_BS * fxWaterAbsorption * absorb);
	vec3 floor_pos = worldPosition + V * below;
	float caustic = fxCaustic(fxSunUV(floor_pos), fxTime * 0.8, 8.0) * fxCaustics * fxSunStrength
		* daylight * (1.0 - exp(-below / FX_BS * 1.5)); // none right at the shoreline
	behind *= 1.0 + caustic * 2.0;
	float F = fresnel * fxReflection * (from_below ? 0.3 : 1.0);
	// the lit water texture is the scattered "body" colour, pulled towards
	// the biome's water (turquoise, deep blue, murky)
	vec3 body = mix(lit * 0.9, fxWaterTint * fxLuma(lit) * 2.6, 0.55);
	transmitted = behind * trans * (1.0 - F);
	vec3 own = body * (1.0 - trans) * (1.0 - F) + refl * F;
	// foam: along shores and around anything standing in the water, and
	// on crests when the water is rough
	if (fxFoam > 0.0 && !from_below) {
		// around anything standing in the water: from the scene depth
		float contact = 1.0 - smoothstep(0.0, 0.3 * FX_BS, below);
		float foam;
		if (top) {
			foam = fxWaterFoam(wabs, wi, wave_h, wave_amp, contact * 0.8, wflow);
		} else {
			float n = fxFbm2(wabs * 2.2 + fxTime * 0.3);
			foam = clamp(contact * smoothstep(0.35, 0.6, n + contact * 0.3) * fxFoam, 0.0, 1.0);
		}
		// foam is white: lit like any surface here (the vertex light),
		// not like the dark water texture
		vec3 light = max(fxToLinear(varColor.rgb) * 0.85, vec3(0.01));
		own = mix(own, light, foam * 0.85);
		transmitted *= 1.0 - foam * 0.85;
	}
	return own;
#else
	// Glass: stained glass tints by its texture colour where it is
	// translucent; opaque texels (frames, leading) keep the lit texture.
	vec3 tint = mix(vec3(1.0), fxToLinear(base.rgb) * 1.3, clamp(base.a * 1.2, 0.0, 1.0));
	float frame = smoothstep(0.85, 0.98, base.a);
	float F = fresnel * fxReflection * (1.0 - frame);
	transmitted = behind * tint * 0.96 * (1.0 - frame) * (1.0 - F);
	vec3 own = lit * frame * (1.0 - F) + refl * F;
	// frost creeping in from the pane's edges while snow lies (weather)
	float cold = fxSnowCover * fxSnow;
	if (cold > 0.001) {
		vec3 a = abs(N);
		vec3 U = a.x > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 W = a.z > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
		vec3 wp = (worldPosition + cameraOffset) / FX_BS;
		vec3 local = wp - floor(wp + 0.5);
		float u = dot(local, U), v = dot(local, W);
		float e = min(min(u + 0.5, 0.5 - u), min(v + 0.5, 0.5 - v));
		float n = fxFbm3(wp * 6.0) - 0.5;
		float m = smoothstep(0.16 * cold, 0.0, e + n * 0.06) * cold;
		vec3 frost = vec3(0.85, 0.9, 1.0) * max(fxLuma(lit) * 1.2, 0.03);
		own = mix(own, frost, m * 0.6);
		transmitted *= 1.0 - m * 0.5;
	}
	return own;
#endif
}
#endif
