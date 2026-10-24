// fx: GPU particles, shared by fx_particles_update, _spawn and _draw.
// C++ side: src/client/fx/particles.{h,cpp} (ParticleBlock: keep the std140
// layout in sync), particle_emit.cpp (what spawns where).
//
// Positions are in nodes relative to the camera offset (fxpAnchor, world
// nodes), so scene space is simply pos * FX_BS. The world texture is laid out
// like the light volume atlas: r solid, g open to the sky, b liquid (1 water,
// ~0.5 lava). Include after uniforms.glsl/color.glsl; define fxpWorldTex
// (a sampler2D) first to get fxpWorldAt.
#include "fx_common/color.glsl"
#if defined(FX_ENABLE) && !defined(FX_PARTICLES_GLSL)
#define FX_PARTICLES_GLSL

layout(std140) uniform FxParticles {
	vec4 fxpFrame;      // dt, time, state width, state height
	vec4 fxpShift;      // anchor change this frame (nodes), wake strength
	vec4 fxpWorld;      // world texture origin relative to the anchor (nodes), valid
	vec4 fxpAnchor;     // the anchor (world nodes)
	vec4 fxpPlayer;     // player centre relative to the anchor (nodes), speed (nodes/s)
	vec4 fxpPlayerVel;  // nodes/s
	vec4 fxpCamRight;   // xyz, w: day-night ratio (0..1)
	vec4 fxpCamUp;      // xyz, w: night (0..1)
	vec4 fxpCamPos;     // camera relative to the anchor (nodes)
	vec4 fxpSky;        // sky light colour (linear), ambient floor
};
#define fxpDt fxpFrame.x
#define fxpStateSize fxpFrame.zw
#define fxpWake fxpShift.w
#define fxpDayNight fxpCamRight.w
#define fxpNight fxpCamUp.w

// particle kinds (src/client/fx/particles.h, ParticleKind)
const float FXP_SPARK = 1.0;
const float FXP_LEAF = 2.0;
const float FXP_RAIN = 3.0;
const float FXP_SPLASH = 4.0;
const float FXP_SNOW = 5.0;
const float FXP_DRIP = 6.0;
const float FXP_DUST = 7.0;
const float FXP_ASH = 8.0;
const float FXP_FIREFLY = 9.0;

bool fxpIs(float kind, float k) { return abs(kind - k) < 0.5; }

// state texel of a slot
ivec2 fxpTexel(float index)
{
	int i = int(index + 0.5);
	int w = int(fxpStateSize.x);
	return ivec2(i - (i / w) * w, i / w);
}

#ifdef fxpWorldTex
// world voxel at p (nodes, relative to the anchor); outside the window: open air
vec4 fxpWorldAt(vec3 p)
{
	if (fxpWorld.w < 0.5)
		return vec4(0.0, 1.0, 0.0, 0.0);
	ivec3 v = ivec3(floor(p + 0.5 - fxpWorld.xyz));
	if (any(lessThan(v, ivec3(0))) ||
			any(greaterThanEqual(v, ivec3(FX_VOLUME_SX, FX_VOLUME_SY, FX_VOLUME_SZ))))
		return vec4(0.0, 1.0, 0.0, 0.0);
	ivec2 t = ivec2((v.y % FX_VOLUME_COLS) * FX_VOLUME_SX + v.x,
			(v.y / FX_VOLUME_COLS) * FX_VOLUME_SZ + v.z);
	return texelFetch(fxpWorldTex, t, 0);
}
#endif

// The wind field foliage sways in (nodes_shader/opengl_vertex.glsl, FX_WIND):
// the same gusts, as an air velocity in nodes/s. `wp` in world nodes.
vec3 fxpWind(vec3 wp)
{
	vec2 wd = vec2(cos(fxWindAngle), sin(fxWindAngle));
	float gust = fxNoise3(vec3(dot(wp.xz, wd) * 0.08 - fxTime * 0.35 * fxWindStrength,
		dot(wp.xz, vec2(-wd.y, wd.x)) * 0.05, 0.0));
	float sway = fxWindStrength * fxWind * (0.25 + gust * gust * 1.6);
	return vec3(wd.x, 0.0, wd.y) * sway * 2.5;
}

// Divergence-free turbulence: the analytic curl of a sum of sines, two
// octaves (the second rotated). About unit amplitude.
vec3 fxpCurlOctave(vec3 p, float t)
{
	const float a = 1.0, b = 1.37;
	vec3 u = vec3(t, t * 1.3 + 1.7, t * 0.7 + 3.1);
	vec3 w = vec3(t * 0.9 + 2.3, t * 1.1 + 0.4, t * 1.2 + 5.2);
	return vec3(
		b * cos(b * p.y + w.z) - a * cos(a * p.z + u.y),
		b * cos(b * p.z + w.x) - a * cos(a * p.x + u.z),
		b * cos(b * p.x + w.y) - a * cos(a * p.y + u.x)) * 0.35;
}

vec3 fxpCurl(vec3 p, float t)
{
	const mat3 R = mat3(0.36, 0.48, -0.8, -0.8, 0.6, 0.0, 0.48, 0.64, 0.6); // orthonormal
	return fxpCurlOctave(p, t) + transpose(R) * fxpCurlOctave(R * p * 2.1, t * 1.7) * 0.5;
}

#endif
