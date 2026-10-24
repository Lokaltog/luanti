// fx: glass and water surfaces (M3). Included by nodes_shader/opengl_fragment.glsl.
#ifdef FX_SURFACE_ACTIVE
/*
	fx: glass and water. The opaque scene was copied (color + depth) before
	the transparent pass; surfaces refract it, tint it and reflect it.
	All of this happens in the same (gamma-ish) space as the scene buffer.
*/
#define fxSceneColor texture4
#define fxSceneDepth texture5
uniform sampler2D fxSceneColor;
uniform sampler2D fxSceneDepth;
uniform float fxSceneValid;
uniform mat4 fxViewProj;       // scene space (relative to cameraOffset) -> clip
uniform vec2 fxNearFar;
uniform vec3 fxSunDir;
uniform float fxSunStrength;
uniform float fxTime;
uniform float fxRefraction;
uniform float fxReflection;
uniform float fxWaterAbsorption;
uniform float fxCaustics;

const float FX_BS = 10.0; // one node in scene units

float fxLinearDepth(float d)
{
	float n = fxNearFar.x, f = fxNearFar.y;
	return 2.0 * n * f / (f + n - (d * 2.0 - 1.0) * (f - n));
}

vec2 fxProject(vec3 p, out float depth)
{
	vec4 c = fxViewProj * vec4(p, 1.0);
	vec3 ndc = c.xyz / c.w;
	depth = ndc.z * 0.5 + 0.5;
	return ndc.xy * 0.5 + 0.5;
}

// Sum-of-warps caustics (after joltz0r's "water turbulence"), bright web of lines.
float fxCaustic(vec2 p, float t)
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
	return clamp(pow(abs(c), 8.0), 0.0, 1.0);
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
	// water: two octaves of moving ripples
	float t = fxTime;
	vec2 q = p.xz * 1.7;
	vec2 g = vec2(cos(q.x * 2.1 + t * 1.3) + cos((q.x + q.y) * 3.3 - t * 1.7) * 0.5,
	              cos(q.y * 2.3 - t * 1.1) + cos((q.x - q.y) * 2.9 + t * 1.9) * 0.5) * 0.06;
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

vec3 fxSampleScene(vec2 uv)
{
#if FX_PATTERN == 2
	// frosted: small disk blur
	vec2 px = 1.0 / vec2(textureSize(fxSceneColor, 0));
	vec3 acc = vec3(0.0);
	const vec2 taps[8] = vec2[8](vec2(1.0, 0.0), vec2(0.7, 0.7), vec2(0.0, 1.0), vec2(-0.7, 0.7),
		vec2(-1.0, 0.0), vec2(-0.7, -0.7), vec2(0.0, -1.0), vec2(0.7, -0.7));
	for (int i = 0; i < 8; i++)
		acc += texture2D(fxSceneColor, uv + taps[i] * px * 5.0).rgb;
	return (acc + texture2D(fxSceneColor, uv).rgb * 2.0) / 10.0;
#else
	return texture2D(fxSceneColor, uv).rgb;
#endif
}

#ifdef FX_SSR
// March the reflected ray against the scene depth copy. Returns rgb + hit weight.
vec4 fxTraceReflection(vec3 origin, vec3 dir)
{
	float stepLen = 0.4 * FX_BS;
	vec3 p = origin + dir * 0.3 * FX_BS;
	vec3 prev = origin;
	for (int i = 0; i < 28; i++) {
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
				for (int j = 0; j < 5; j++) {
					vec3 m = (a + b) * 0.5;
					float md;
					vec2 muv = fxProject(m, md);
					if (md > texture2D(fxSceneDepth, muv).r) b = m; else a = m;
				}
				float bd;
				vec2 huv = fxProject(b, bd);
				vec2 edge = min(huv, 1.0 - huv);
				float fade = clamp(min(edge.x, edge.y) * 12.0, 0.0, 1.0)
					* (1.0 - float(i) / 28.0);
				return vec4(texture2D(fxSceneColor, huv).rgb, fade);
			}
			break; // went behind something thick: no reliable hit
		}
		prev = p;
		p += dir * stepLen;
		stepLen *= 1.25;
	}
	return vec4(0.0);
}
#endif

vec3 fxSurface(vec3 col, vec4 base)
{
	vec2 screen = vec2(textureSize(fxSceneColor, 0));
	vec2 suv = gl_FragCoord.xy / screen;
	vec3 eye = cameraPosition - cameraOffset;
	vec3 V = normalize(worldPosition - eye);
	vec3 N = normalize(vNormal);
	bool from_below = dot(N, V) > 0.0;
	if (from_below)
		N = -N;
	vec3 dn = fxPatternNormal(N, worldPosition);

	// Screen-space derivative of the texture's luminance bends light a little
	// too, so streaky glass textures distort without any pattern.
	float lum = dot(base.rgb, vec3(0.3, 0.6, 0.1)) * (1.0 - base.a * 0.5);
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
	vec2 ruv = suv + (p1 - p0) + tex_bend * fxRefraction;
	// Never refract in something that is in front of the surface
	if (texture2D(fxSceneDepth, ruv).r < gl_FragCoord.z)
		ruv = suv;
	vec3 behind = fxSampleScene(ruv);

	// Reflection
	vec3 Np = normalize(N + dn);
	vec3 R = reflect(V, Np);
	float cosv = clamp(dot(-V, Np), 0.0, 1.0);
	float fresnel = 0.04 + 0.96 * pow(1.0 - cosv, 5.0);
	float daylight = clamp(1.0 - nightRatio, 0.15, 1.0);
	vec3 sky = mix(fogColor.rgb, fogColor.rgb * vec3(0.75, 0.85, 1.15), clamp(R.y, 0.0, 1.0)) * daylight;
	vec3 refl = sky;
#ifdef FX_SSR
	if (!from_below) {
		vec4 hit = fxTraceReflection(worldPosition, R);
		refl = mix(sky, hit.rgb, hit.a);
	}
#endif

#if FX_SURFACE == 2
	// Water: absorption by depth (red goes first), caustics on what's below
	float below = max(fxLinearDepth(texture2D(fxSceneDepth, ruv).r) - fxLinearDepth(gl_FragCoord.z), 0.0);
	if (from_below)
		below = 0.0; // looking up from inside: the post pass handles absorption
	vec3 trans = exp(-below / FX_BS * fxWaterAbsorption * vec3(1.6, 0.6, 0.35));
	vec3 floor_pos = worldPosition + V * below;
	vec2 cp = (floor_pos.xz - fxSunDir.xz / max(fxSunDir.y, 0.2) * floor_pos.y) / FX_BS * 0.9;
	float caustic = fxCaustic(cp, fxTime * 0.8) * fxCaustics * fxSunStrength * daylight
		* (1.0 - exp(-below / FX_BS * 1.5)); // none right at the shoreline
	behind *= 1.0 + caustic * 1.5;
	// col carries the lit water texture: it becomes the scattered "body" colour
	vec3 body = col * 0.9;
	vec3 water = mix(body, behind, trans);
	return mix(water, refl, fresnel * fxReflection * (from_below ? 0.3 : 1.0));
#else
	// Glass: stained glass tints by its texture colour where it is translucent;
	// opaque texels (frames, leading) keep the normal lit texture.
	vec3 tint = mix(vec3(1.0), base.rgb * 1.15, clamp(base.a * 1.2, 0.0, 1.0));
	vec3 glass = behind * tint * 0.96;
	float frame = smoothstep(0.85, 0.98, base.a);
	vec3 surf = mix(glass, col, frame);
	return mix(surf, refl, fresnel * fxReflection * (1.0 - frame));
#endif
}
#endif
