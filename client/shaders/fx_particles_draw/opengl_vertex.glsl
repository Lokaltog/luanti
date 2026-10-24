// fx: GPU particle draw. A static mesh of 16384 quads per chunk of slots;
// TCoords = (quad index within the chunk, corner), the chunk index comes in
// materialColor.rg. The vertex fetches its particle's state and builds the
// quad: velocity-stretched streaks (sparks, rain, drips), tumbling or flat
// leaves, flat splash rings, camera-facing dots (snow, dust, ash, fireflies).
// Lighting per particle: sky (open to the sky, day-night ratio) and the
// light volume's artificial light; glowing kinds add HDR emission.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#define fxpS0 texture0
#define fxpS1 texture1
#define fxpS2 texture2
#define fxpS3 texture3
#define fxpLightVolume texture6
#define fxpWorldTex texture7
uniform sampler2D fxpS0;
uniform sampler2D fxpS1;
uniform sampler2D fxpS2;
uniform sampler2D fxpS3;
uniform sampler2D fxpLightVolume;
uniform sampler2D fxpWorldTex;
uniform vec4 materialColor;
#include "fx_common/particles.glsl"

VARYING_ vec2 vUV;          // -1..1 across the quad
VARYING_ float vDepth;      // view depth (scene units)
flat VARYING_ vec4 vColor;  // linear rgb (light x albedo, or emission), alpha
flat VARYING_ vec4 vInfo;   // kind, leaf layer, occlusion (0 = additive), soft range (nodes)
flat VARYING_ vec3 vTint;   // linear tint (leaves: x texture), lit

const vec3 VOL_SIZE = vec3(float(FX_VOLUME_SX), float(FX_VOLUME_SY), float(FX_VOLUME_SZ));
const float VOL_COLS = float(FX_VOLUME_COLS);
const vec2 VOL_ATLAS = vec2(VOL_SIZE.x * VOL_COLS, VOL_SIZE.z * ceil(VOL_SIZE.y / VOL_COLS));

// artificial light levels (0..1 = 0..15) at a scene-space position
vec3 artificialAt(vec3 sp)
{
	if (fxVolumeValid < 0.5)
		return vec3(0.0);
	vec3 v = (sp - fxVolumeOrigin) / FX_BS;
	if (any(lessThan(v, vec3(-0.5))) || any(greaterThan(v, VOL_SIZE - 0.5)))
		return vec3(0.0);
	float y = clamp(v.y, 0.0, VOL_SIZE.y - 1.0);
	float y0 = floor(y);
	float y1 = min(y0 + 1.0, VOL_SIZE.y - 1.0);
	vec2 xz = clamp(v.xz, vec2(0.0), VOL_SIZE.xz - 1.0) + 0.5;
	vec2 t0 = (vec2(mod(y0, VOL_COLS), floor(y0 / VOL_COLS)) * VOL_SIZE.xz + xz) / VOL_ATLAS;
	vec2 t1 = (vec2(mod(y1, VOL_COLS), floor(y1 / VOL_COLS)) * VOL_SIZE.xz + xz) / VOL_ATLAS;
	return mix(textureLod(fxpLightVolume, t0, 0.0).rgb, textureLod(fxpLightVolume, t1, 0.0).rgb, y - y0);
}

void cull()
{
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume, zero area
	vUV = vec2(0.0);
	vDepth = 0.0;
	vColor = vec4(0.0);
	vInfo = vec4(0.0);
	vTint = vec3(0.0);
}

void main(void)
{
	int chunk = int(materialColor.r * 255.0 + 0.5) + 256 * int(materialColor.g * 255.0 + 0.5);
	float index = float(chunk) * 16384.0 + inTexCoord0.x;
	ivec2 tc = fxpTexel(index);
	if (tc.y >= int(fxpStateSize.y)) {
		cull();
		return;
	}
	vec4 s1 = texelFetch(fxpS1, tc, 0);
	float kind = s1.w;
	if (kind < 0.5) {
		cull();
		return;
	}
	vec4 s0 = texelFetch(fxpS0, tc, 0);
	vec4 s2 = texelFetch(fxpS2, tc, 0);
	vec4 s3 = texelFetch(fxpS3, tc, 0);
	int corner = int(inTexCoord0.y + 0.5);
	vec2 c = vec2(corner == 1 || corner == 2 ? 1.0 : -1.0, corner >= 2 ? 1.0 : -1.0);

	vec3 p = s0.xyz;
	vec3 vel = s1.xyz;
	float age = s0.w, life = max(s2.x, 1e-3), seed = s2.y, param = s2.z, rest = s2.w;
	float f = clamp(age / life, 0.0, 1.0);
	float size = s3.z, glow = s3.w, layer = s3.y;
	float tint_code = s3.x;
	vec3 tint = vec3(floor(tint_code / 65536.0), mod(floor(tint_code / 256.0), 256.0), mod(tint_code, 256.0)) / 255.0;
	tint = fxToLinear(tint);

	vec3 right = fxpCamRight.xyz, up = fxpCamUp.xyz;
	vec3 to_cam = fxpCamPos.xyz - p;
	float dist = length(to_cam);
	to_cam /= max(dist, 1e-4);

	bool spark = fxpIs(kind, FXP_SPARK), leaf = fxpIs(kind, FXP_LEAF);
	bool rain = fxpIs(kind, FXP_RAIN), drip = fxpIs(kind, FXP_DRIP);
	bool snow = fxpIs(kind, FXP_SNOW), splash = fxpIs(kind, FXP_SPLASH);
	bool dust = fxpIs(kind, FXP_DUST), ash = fxpIs(kind, FXP_ASH), firefly = fxpIs(kind, FXP_FIREFLY);

	vec3 offset;
	if (spark || rain || drip) {
		// a streak from where it was a moment ago to where it is
		float streak = spark ? 0.05 : 0.03;
		vec3 along = vel * streak;
		float len = length(along);
		vec3 axis = len > 1e-4 ? along / len : up;
		vec3 side = cross(axis, to_cam);
		float sl = length(side);
		side = sl > 1e-4 ? side / sl : right;
		float half_len = len * 0.5 + size;
		offset = -along * 0.5 + axis * c.y * half_len + side * c.x * size;
	} else if (leaf) {
		if (rest >= 0.0) {
			// flat on the face, turned at random
			float a = seed * 6.2832;
			vec3 u = vec3(cos(a), 0.0, sin(a)), w = vec3(-sin(a), 0.0, cos(a));
			offset = (u * c.x + w * c.y) * size + vec3(0.0, 0.008, 0.0);
		} else {
			// tumbling: the leaf's normal swings and spins
			float spin = param * 3.0;
			float a1 = age * spin + seed * 6.2832;
			float a2 = age * (1.2 + seed) + seed * 3.0;
			vec3 n = normalize(vec3(sin(a1) * cos(a2), 0.6 + 0.4 * cos(a1), sin(a1) * sin(a2)));
			vec3 u = normalize(cross(n, abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)));
			vec3 w = cross(n, u);
			offset = (u * c.x + w * c.y) * size;
		}
	} else if (splash) {
		float r = (0.04 + 0.2 * f) * param;
		offset = vec3(c.x, 0.0, c.y) * r + vec3(0.0, 0.01, 0.0);
	} else {
		float s = firefly ? size * 3.5 : size;
		offset = (right * c.x + up * c.y) * s;
	}

	vec3 sp = (p + offset) * FX_BS;
	vec4 clip = fxViewProj * vec4(sp, 1.0);
	gl_Position = clip;
	vDepth = clip.w;
	vUV = c;

	// light at the particle: sky (open to the sky, day-night) and the light
	// volume's artificial light, both decoded like display values
	vec4 w = fxpWorldAt(p);
	float sky_level = fxpDayNight * mix(0.3, 1.0, w.g);
	vec3 light = fxToLinear(vec3(sky_level)) * fxpSky.rgb;
	light = max(light, fxToLinear(artificialAt(p * FX_BS)));
	light += vec3(fxpSky.w);

	float fade = 1.0 - smoothstep(38.0, 46.0, dist); // the edge of the simulated window
	vec3 col = vec3(0.0);
	float alpha = 1.0, occl = 1.0, soft = 0.4;
	if (spark) {
		// cooling: white-yellow -> orange -> dull red, HDR so it blooms
		vec3 hot = vec3(1.0, 0.9, 0.6) * 9.0;
		vec3 mid = vec3(1.0, 0.38, 0.06) * 3.5;
		vec3 cold = vec3(0.5, 0.05, 0.01) * 0.7;
		col = (f < 0.3 ? mix(hot, mid, f / 0.3) : mix(mid, cold, (f - 0.3) / 0.7)) * glow;
		alpha = 1.0 - smoothstep(0.8, 1.0, f);
		occl = 0.0;
		soft = 0.15;
	} else if (leaf) {
		col = light;
		vTint = tint;
		alpha = clamp((life - age) / 2.0, 0.0, 1.0) * clamp(age * 4.0, 0.0, 1.0);
		soft = rest >= 0.0 ? 0.0 : 0.2;
	} else if (rain || drip) {
		col = light * vec3(0.6, 0.68, 0.78) + fxToLinear(vec3(sky_level)) * fxpSky.rgb * 0.15;
		alpha = rain ? 0.45 : 0.55;
		occl = 0.5;
		soft = 0.1;
	} else if (splash) {
		col = light * 0.85;
		alpha = (1.0 - f) * 0.55;
		occl = 0.5;
		soft = 0.0;
	} else if (snow) {
		col = light * 0.95;
		alpha = 0.9 * clamp((life - age) / 1.5, 0.0, 1.0);
		soft = rest >= 0.0 ? 0.0 : 0.2;
	} else if (dust) {
		// motes catch the light: brighter where it is bright
		col = light * tint * 0.9;
		alpha = 0.45 * sin(3.14159 * f);
		occl = 0.0;
		soft = 0.2;
	} else if (ash) {
		col = light * tint * 0.8 + tint * glow * 1.8;
		alpha = 0.8 * sin(3.14159 * f);
		occl = glow > 0.0 ? 0.0 : 0.6;
		soft = 0.2;
	} else if (firefly) {
		// blinking: on for a moment, then dark
		float blink = smoothstep(0.55, 0.95, sin(age * 2.1 + seed * 31.0) * 0.5 + 0.5);
		col = tint * 4.0 * blink * glow;
		alpha = clamp(min(age, life - age), 0.0, 1.0);
		occl = 0.0;
		soft = 0.2;
	}
	vColor = vec4(col, alpha * fade);
	vInfo = vec4(kind, layer, occl, soft);
	if (!leaf)
		vTint = vec3(1.0);
}
