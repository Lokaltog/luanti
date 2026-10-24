// fx: artificial light hue and flicker for node and object vertex shaders.
// Included by nodes_shader and object_shader opengl_vertex.glsl after the
// declaration of `artificialLight`. The volume is bound to map materials and
// to entity materials (GenericCAO); where it isn't bound the sampler reads
// zero, which leaves the light white.
#include "fx_common/uniforms.glsl"

#if defined(FX_ENABLE) && defined(FX_COLORED_LIGHT)
// fx: nearest light sources this frame, positions relative to cameraOffset (BS units)
VARYING_ vec3 fxChroma; // for debug view 1
float fxHeldLevel = 0.0;  // held light level here (0..1 = 0..15), set by fxArtificialLight
vec3 fxHeldHue = vec3(1.0);
float fxArtLevel = 0.0;   // coloured world light here (0..1), set by fxArtificialLight
vec3 fxArtHue = vec3(1.0); // its hue, luminance-normalised

#ifdef FX_LIGHT_VOLUME
// fx M4: leak-free RGB light volume, Y slices tiled 8 per row in a 2D atlas
#define fxLightVolume texture6
uniform sampler2D fxLightVolume;
// sizes come from src/client/fx/light_volume.h (FX_VOLUME_* constants)
const vec3 FX_VOL_SIZE = vec3(float(FX_VOLUME_SX), float(FX_VOLUME_SY), float(FX_VOLUME_SZ));
const float FX_VOL_COLS = float(FX_VOLUME_COLS);
// the texture is two atlases stacked: light levels on top, the sky tint
// (daylight coloured by stained glass it came through) below
const vec2 FX_VOL_ATLAS = vec2(FX_VOL_SIZE.x * FX_VOL_COLS,
	FX_VOL_SIZE.z * ceil(FX_VOL_SIZE.y / FX_VOL_COLS));
const vec2 FX_VOL_TEX = FX_VOL_ATLAS * vec2(1.0, 2.0);

vec2 fxVolumeTexel(float slice, vec2 xz)
{
	return (vec2(mod(slice, FX_VOL_COLS), floor(slice / FX_VOL_COLS)) * FX_VOL_SIZE.xz + xz) / FX_VOL_TEX;
}

// rgb = light level per channel (0..1 = 0..15), a = the held light's level;
// all zero outside the window
vec4 fxVolumeSample(vec3 p)
{
	vec3 v = (p - fxVolumeOrigin) / FX_BS;
	if (any(lessThan(v, vec3(-0.5))) || any(greaterThan(v, FX_VOL_SIZE - 0.5)))
		return vec4(0.0);
	float y = clamp(v.y, 0.0, FX_VOL_SIZE.y - 1.0);
	float y0 = floor(y);
	float y1 = min(y0 + 1.0, FX_VOL_SIZE.y - 1.0);
	// bilinear inside a slice by the hardware, clamped so it never reads a
	// neighbouring slice; linear between slices by hand
	vec2 xz = clamp(v.xz, vec2(0.0), FX_VOL_SIZE.xz - 1.0) + 0.5;
	return mix(textureLod(fxLightVolume, fxVolumeTexel(y0, xz), 0.0),
		textureLod(fxLightVolume, fxVolumeTexel(y1, xz), 0.0), y - y0);
}

// 1 inside the window, fading to 0 over its last 8 nodes (light there may
// come from outside: the hue blends to white rather than pop as it moves)
float fxVolumeTrust(vec3 p)
{
	vec3 v = (p - fxVolumeOrigin) / FX_BS;
	vec3 edge = min(v, FX_VOL_SIZE - 1.0 - v);
	return smoothstep(0.0, 8.0, min(edge.x, edge.z)) * smoothstep(0.0, 4.0, edge.y);
}

// Daylight's colour at p: white, except where it came through stained
// glass. Linear-ish multiplier for the day share of the light.
vec3 fxDaylightTint(vec3 p)
{
	vec3 v = (p - fxVolumeOrigin) / FX_BS;
	if (any(lessThan(v, vec3(-0.5))) || any(greaterThan(v, FX_VOL_SIZE - 0.5)))
		return vec3(1.0);
	float y = clamp(v.y, 0.0, FX_VOL_SIZE.y - 1.0);
	float y0 = floor(y), y1 = min(y0 + 1.0, FX_VOL_SIZE.y - 1.0);
	vec2 xz = clamp(v.xz, vec2(0.0), FX_VOL_SIZE.xz - 1.0) + 0.5;
	const vec2 lower = vec2(0.0, 0.5);
	vec4 t = mix(textureLod(fxLightVolume, fxVolumeTexel(y0, xz) + lower, 0.0),
		textureLod(fxLightVolume, fxVolumeTexel(y1, xz) + lower, 0.0), y - y0);
	// alpha 1: written by the fill (an unbound sampler reads alpha 1 too,
	// with black: guard on the colour)
	if (t.a < 0.5 || max(t.r, max(t.g, t.b)) <= 0.0)
		return vec3(1.0);
	vec3 tint = t.rgb * t.rgb; // display-ish to linear-ish: richer colour
	return mix(vec3(1.0), tint, fxVolumeTrust(p));
}

#endif

// Hue (and flicker) of the artificial light at p. Intensity is NOT changed here:
// the engine's flood-filled scalar light stays authoritative.
// The hue of world lights comes from the leak-free RGB light volume; outside
// it (or before its first fill) light is white, like upstream. The light
// list carries only what changes per frame: flickering sources (amplitude,
// weighted by distance) and the held light, whose hue is added near it.
vec3 fxArtificialLight(vec3 p, vec3 n, float seconds)
{
	float flick = 0.0;
	float fsum = 0.0;
	vec3 held_acc = vec3(0.0);
	float held_w = 0.0;
	for (int i = 0; i < FX_LIGHT_MAX; i++) {
		if (i >= fxLightCount)
			break;
		vec4 pr = fxLightPosRadius[i];
		vec3 d = pr.xyz - p;
		float dist2 = dot(d, d);
		float r2 = pr.w * pr.w;
		if (dist2 >= r2)
			continue;
		float fall = 1.0 - dist2 / r2;
		// sharper ownership near a light: inverse square in nodes (BS = 10)
		float w = fall * fall / (dist2 * 0.01 + 1.0);
		vec4 cs = fxLightColorSeed[i];
		if (i == fxHeldLightIndex) {
			held_acc += w * cs.rgb;
			held_w += w;
			fxHeldHue = cs.rgb;
		}
		float amp = floor(cs.a) * 0.01;
		float seed = fract(cs.a) * 97.0;
		float nz = sin(seconds * 7.3 + seed) * 0.5
			+ sin(seconds * 13.7 + seed * 1.7) * 0.3
			+ sin(seconds * 23.1 + seed * 2.3) * 0.2;
		flick += w * amp * nz;
		fsum += w;
	}
	vec3 acc = held_acc;
	float wsum = held_w;
#ifdef FX_LIGHT_VOLUME
	if (fxVolumeValid > 0.5) {
		vec3 q = p + n * 5.0; // half a node in front of the face
		vec4 vs = fxVolumeSample(q);
		// alpha 1 can't come from the atlas (held levels stop at 14/15): an
		// unbound sampler (the wield scene, other objects) reads (0,0,0,1)
		// The flood fill (leak-free, but refreshed only every ~0.1 s at
		// the node the camera was in) bounds a smooth analytic falloff from
		// where the camera is now: the light glides with you, walls still
		// stop it.
		float bfs = vs.a > 0.99 ? 0.0 : vs.a * 15.0;
		float analytic = float(fxHeldLevelMax) - length(q - fxEyePos) / FX_BS;
		fxHeldLevel = fxHeldLevelMax > 0 ? clamp(min(analytic, bfs + 1.5), 0.0, 15.0) / 15.0 : 0.0;
		float m = max(vs.r, max(vs.g, vs.b));
		fxArtLevel = m * fxVolumeTrust(q);
		if (m > 0.0) {
			// world lights from the volume, weighted by how strong they are here
			float vw = m * 4.0;
			acc += mix(vec3(1.0), vs.rgb / m, fxVolumeTrust(q)) * vw;
			wsum += vw;
		}
	}
#endif
	float f = 1.0 + (fsum > 0.0 ? flick / fsum : 0.0);
	fxHeldLevel *= f; // the held torch flickers too
	if (wsum <= 1e-5) {
		fxChroma = vec3(1.0);
		return artificialLight * f;
	}
	vec3 chroma = acc / wsum;
	fxChroma = chroma;
	// keep the luminance of upstream's white light so exposure is unchanged
	float lum = dot(chroma, vec3(0.2126, 0.7152, 0.0722));
	chroma = min(chroma * (1.04 / max(lum, 0.05)), vec3(2.4));
	fxArtHue = chroma / 1.04;
	return mix(artificialLight, chroma, fxLightSaturation) * f;
}
// The day share of the light: the sun's colour, tinted by stained glass it
// came through, and near coloured lamps a little of their hue (the engine
// keeps only the brighter of sun and lamp, so in daylight a torch's colour
// would vanish entirely; sunlight is far stronger, so the lamp only shows
// where the day light is weak: shade, indoors). sky: the day light level
// here (0..1).
vec3 fxDayLight(vec3 day, vec3 p, vec3 n, float sky)
{
#ifdef FX_LIGHT_VOLUME
	if (fxVolumeValid > 0.5) {
		vec3 q = p + n * 5.0;
		day *= fxDaylightTint(q);
		float a = fxArtLevel, s = sky * 2.5;
		float w = a * a / (a * a + s * s + 1e-4) * fxLightSaturation;
		day *= mix(vec3(1.0), fxArtHue, w);
	}
#endif
	return day;
}

#ifdef FX_LIGHT_VOLUME
// Brightness of the held light here (after fxArtificialLight): the engine
// has none, so it comes from the volume's alpha, combined like the engine
// combines lights (max). Display-encoded, like the vertex colour.
vec3 fxHeldLight()
{
	if (fxHeldLevel <= 0.0)
		return vec3(0.0);
	float l = clamp(fxHeldLevel * 15.0 / 14.0, 0.0, 1.0);
	float lum = dot(fxHeldHue, vec3(0.2126, 0.7152, 0.0722));
	vec3 hue = min(fxHeldHue * (1.04 / max(lum, 0.05)), vec3(2.4));
	return mix(vec3(1.04), hue, fxLightSaturation) * l * l;
}
#endif
#endif
