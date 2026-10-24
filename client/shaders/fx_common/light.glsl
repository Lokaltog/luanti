// fx: artificial light hue and flicker for node and object vertex shaders.
// Included by nodes_shader and object_shader opengl_vertex.glsl after the
// declaration of `artificialLight`. Object shaders #undef FX_LIGHT_VOLUME
// first: the volume is only bound to map materials.
#ifdef FX_ENABLE
uniform float fxAmbientFloor;
#endif

#if defined(FX_ENABLE) && defined(FX_COLORED_LIGHT)
// fx: nearest light sources this frame, positions relative to cameraOffset (BS units)
uniform int fxLightCount;
uniform vec4 fxLightPosRadius[FX_LIGHT_MAX]; // xyz = position, w = radius
uniform vec4 fxLightColorSeed[FX_LIGHT_MAX]; // rgb = chroma, a = floor: flicker amp*100, fract: seed
uniform float fxLightSaturation;
VARYING_ vec3 fxChroma; // for debug view 1

#ifdef FX_LIGHT_VOLUME
// fx M4: leak-free RGB light volume, Y slices tiled 8 per row in a 2D atlas
#define fxLightVolume texture6
uniform sampler2D fxLightVolume;
uniform vec3 fxVolumeOrigin;  // scene-space centre of voxel (0,0,0)
uniform float fxVolumeValid;
uniform int fxHeldLightIndex;
const vec3 FX_VOL_SIZE = vec3(64.0, 48.0, 64.0);
const vec2 FX_VOL_ATLAS = vec2(64.0 * 8.0, 64.0 * 6.0);

vec2 fxVolumeTexel(float slice, vec2 xz)
{
	return (vec2(mod(slice, 8.0), floor(slice / 8.0)) * FX_VOL_SIZE.xz + xz) / FX_VOL_ATLAS;
}

// rgb = light level per channel (0..1 = 0..15), a = 1 inside the volume
vec4 fxVolumeSample(vec3 p)
{
	vec3 v = (p - fxVolumeOrigin) / 10.0;
	if (any(lessThan(v, vec3(-0.5))) || any(greaterThan(v, FX_VOL_SIZE - 0.5)))
		return vec4(0.0);
	float y = clamp(v.y, 0.0, FX_VOL_SIZE.y - 1.0);
	float y0 = floor(y);
	float y1 = min(y0 + 1.0, FX_VOL_SIZE.y - 1.0);
	// bilinear inside a slice by the hardware, clamped so it never reads a
	// neighbouring slice; linear between slices by hand
	vec2 xz = clamp(v.xz, vec2(0.0), FX_VOL_SIZE.xz - 1.0) + 0.5;
	vec3 c0 = textureLod(fxLightVolume, fxVolumeTexel(y0, xz), 0.0).rgb;
	vec3 c1 = textureLod(fxLightVolume, fxVolumeTexel(y1, xz), 0.0).rgb;
	return vec4(mix(c0, c1, y - y0), 1.0);
}
#endif

// Hue (and flicker) of the artificial light at p. Intensity is NOT changed here:
// the engine's flood-filled scalar light stays authoritative.
// With the light volume (M4) the hue of world lights comes from the leak-free
// RGB flood fill; the light list still provides flicker and the held light.
// Without it (T1), the hue is a distance-weighted mix of nearby lights and
// can bleed through walls.
vec3 fxArtificialLight(vec3 p, vec3 n, float seconds)
{
	vec3 acc = vec3(0.0);
	float wsum = 0.0;
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
		float amp = floor(cs.a) * 0.01;
		float seed = fract(cs.a) * 97.0;
		float nz = sin(seconds * 7.3 + seed) * 0.5
			+ sin(seconds * 13.7 + seed * 1.7) * 0.3
			+ sin(seconds * 23.1 + seed * 2.3) * 0.2;
		acc += w * cs.rgb;
		wsum += w;
		flick += w * amp * nz;
		fsum += w;
#ifdef FX_LIGHT_VOLUME
		if (i == fxHeldLightIndex) {
			held_acc += w * cs.rgb;
			held_w += w;
		}
#endif
	}
#ifdef FX_LIGHT_VOLUME
	if (fxVolumeValid > 0.5) {
		vec4 vs = fxVolumeSample(p + n * 5.0); // half a node in front of the face
		if (vs.a > 0.0) {
			float m = max(vs.r, max(vs.g, vs.b));
			// world lights from the volume, weighted by how strong they are here
			float vw = m * 4.0;
			acc = (m > 0.001 ? vs.rgb / m : vec3(1.0)) * vw + held_acc;
			wsum = vw + held_w;
		}
	}
#endif
	if (wsum <= 1e-5) {
		fxChroma = vec3(1.0);
		return artificialLight;
	}
	vec3 chroma = acc / wsum;
	fxChroma = chroma;
	// keep the luminance of upstream's white light so exposure is unchanged
	float lum = dot(chroma, vec3(0.2126, 0.7152, 0.0722));
	chroma = min(chroma * (1.04 / max(lum, 0.05)), vec3(2.4));
	return mix(artificialLight, chroma, fxLightSaturation) * (1.0 + (fsum > 0.0 ? flick / fsum : 0.0));
}
#endif
