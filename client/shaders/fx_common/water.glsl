// fx: water waves sized to the body of water. Included by the node shaders
// (vertex: the swell displacement; fragment: normals and foam).
//
// The water field (fx/water.cpp, texture7 on water) says, per column around
// the camera, how far the shore is, how open the water is and how deep it
// is. Waves are built in layers:
//   swells   three long Gerstner-style sines around the wind direction,
//            turning to run at the shore as they come in, as long and as
//            high as the water is open (a pond has none, the sea rolls)
//   chop     two octaves of value noise drifting with the wind
//   ripples  fine noise, faded out where a pixel covers several of them
// Heights and slopes are in nodes. Choppiness (fxWaterChop) comes from the
// biome and the weather.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#if defined(FX_ENABLE) && defined(FX_SURFACES) && defined(FX_SURFACE) && FX_SURFACE == 2
#define FX_WATER_ACTIVE 1
#define fxWaterField texture7
uniform sampler2D fxWaterField;

#define fxWaterFieldOrigin vec3(fxWater.y, fxWater.w, fxWater.z) // nodes, rel. camera offset

struct FxWaterInfo {
	float shore;    // nodes to the shore along the surface
	float open;     // the body's largest shore distance (nodes): its size
	float depth;    // nodes of water below the surface
	vec2 to_shore;  // unit direction towards the nearest shore (xz)
	float known;    // 1 where the field covers this spot
	float flowing;  // 1 on flowing water (rivers, streams, falls), 0 still
};

// p: node position relative to the camera offset
FxWaterInfo fxWaterAt(vec3 p)
{
	FxWaterInfo w;
	w.shore = 32.0;
	w.open = 12.0;
	w.depth = 4.0;
	w.to_shore = vec2(0.0);
	w.known = 0.0;
	w.flowing = 0.0;
	if (fxWater.w > 1e8)
		return w;
	vec3 o = fxWaterFieldOrigin;
	vec2 uv = (p.xz - o.xz + 0.5) / 96.0;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return w;
	vec4 c = textureLod(fxWaterField, uv, 0.0);
	// the column's surface must be this one (not a pool far below)
	vec4 cn = texelFetch(fxWaterField, ivec2(clamp(floor(p.xz - o.xz + 0.5), 0.0, 95.0)), 0);
	float sy = o.y + cn.a * 64.0 - 1.0;
	if (cn.a <= 0.0 || abs(p.y - sy) > 1.2)
		return w;
	w.known = 1.0;
	w.shore = c.r * 32.0;
	w.open = cn.g * 32.0;
	float b = cn.b * 255.0;
	w.flowing = step(127.5, b);
	w.depth = mod(b, 128.0) / 127.0 * 16.0;
	const float e = 1.0 / 96.0;
	vec2 g = vec2(textureLod(fxWaterField, uv + vec2(e, 0.0), 0.0).r - textureLod(fxWaterField, uv - vec2(e, 0.0), 0.0).r,
	              textureLod(fxWaterField, uv + vec2(0.0, e), 0.0).r - textureLod(fxWaterField, uv - vec2(0.0, e), 0.0).r);
	w.to_shore = dot(g, g) > 1e-8 ? -normalize(g) : vec2(0.0);
	return w;
}

// value noise with its gradient: (value, d/dx, d/dy)
vec3 fxVNoise2d(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
	float a = fxHash13(vec3(i, 5.0)), b = fxHash13(vec3(i + vec2(1.0, 0.0), 5.0));
	float c = fxHash13(vec3(i + vec2(0.0, 1.0), 5.0)), d = fxHash13(vec3(i + vec2(1.0, 1.0), 5.0));
	float k1 = b - a, k2 = c - a, k3 = a - b - c + d;
	return vec3(a + k1 * u.x + k2 * u.y + k3 * u.x * u.y,
	            du.x * (k1 + k3 * u.y), du.y * (k2 + k3 * u.x));
}

// how big this body's waves are: 0 for a puddle or pond, 1 for the sea
float fxWaterScale(FxWaterInfo w)
{
	return smoothstep(1.5, 14.0, w.open);
}

// The direction the swells run: with the wind out on open water, turning to
// face the shore as they come in (refraction over the shallows)
vec2 fxSwellDir(FxWaterInfo w)
{
	vec2 wd = vec2(cos(fxWindAngle), sin(fxWindAngle));
	if (w.known < 0.5 || dot(w.to_shore, w.to_shore) < 0.5)
		return wd;
	return normalize(mix(w.to_shore, wd, smoothstep(1.5, 9.0, w.shore)) + 1e-4);
}

// Swells only (the long waves; the vertex shader moves the mesh with them):
// (height, dh/dx, dh/dz), and the largest possible height in amp_max
vec3 fxSwells(vec2 p, FxWaterInfo w, out float amp_max)
{
	float s = fxWaterScale(w);
	float chop = fxWaterChop;
	// calm at the very edge, a little higher where they pile up in the
	// shallows before breaking
	float shore_k = smoothstep(0.2, 1.6, w.shore) * (1.0 + 0.5 * s * (1.0 - smoothstep(1.5, 5.0, w.shore)))
		* (1.0 - 0.85 * w.flowing); // running water doesn't swell
	float size = mix(0.3, 1.0, s);
	vec2 dir = fxSwellDir(w);
	float t = fxTime;
	vec3 acc = vec3(0.0);
	amp_max = 0.0;
	// wavelength (nodes), angle off the main direction, phase
	const vec3 W0 = vec3(7.0, 0.0, 0.0), W1 = vec3(4.3, 0.55, 1.7), W2 = vec3(2.7, -0.75, 4.1);
	for (int i = 0; i < 3; i++) {
		vec3 wv = i == 0 ? W0 : (i == 1 ? W1 : W2);
		float lambda = wv.x * size;
		float k = 6.2831853 / lambda;
		float ca = cos(wv.y), sa = sin(wv.y);
		vec2 d = vec2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
		float omega = sqrt(9.8 * k) * 0.55;
		float A = lambda * 0.03 * chop * mix(0.05, 1.0, s) * shore_k;
		float ph = dot(d, p) * k - omega * t + wv.z;
		acc += vec3(A * sin(ph), A * k * d * cos(ph));
		amp_max += A;
	}
	return acc;
}

// Rain on the water: per half-node cell a drop lands at a random spot and
// time, and a ring runs out from it. Returns the slope (d/dx, d/dz).
vec2 fxWaterRain(vec2 p)
{
	vec2 acc = vec2(0.0);
	for (int k = 0; k < 3; k++) {
		vec2 q = p * 2.0 + float(k) * vec2(0.37, 0.71);
		vec2 cell = floor(q), f = fract(q);
		float h = fxHash13(vec3(cell, 11.0 + float(k)));
		vec2 c = vec2(fxHash13(vec3(cell, 13.0 + float(k))), fxHash13(vec3(cell, 17.0 + float(k)))) * 0.6 + 0.2;
		float t = fract(fxTime * 1.1 + h);
		vec2 d = f - c;
		float r = length(d);
		// a ring and a fainter one behind it, both dying out as they spread
		float x = (r - t * 0.42) * 30.0, x2 = (r - t * 0.3) * 30.0;
		float ring = (exp(-x * x) - 0.5 * exp(-x2 * x2)) * (1.0 - t) * (1.0 - t);
		acc += d / max(r, 1e-3) * ring * 2.0;
	}
	return acc * 0.5;
}

// All layers for the fragment: (height, dh/dx, dh/dz). footprint: nodes per
// pixel, so detail finer than a pixel fades out instead of shimmering.
// flow: the surface's downhill slope (the liquid mesh tilts its normal that
// way, xz), zero on still water.
vec3 fxWaterWaves(vec2 p, FxWaterInfo w, float footprint, vec2 flow, out float amp_max)
{
	vec3 h = fxSwells(p, w, amp_max);
	float s = fxWaterScale(w);
	float chop = fxWaterChop;
	vec2 wd = vec2(cos(fxWindAngle), sin(fxWindAngle));
	float t = fxTime;
	// running water: the noise rides downstream, stretched along the flow
	// into streaks; still water: it drifts with the wind
	float speed = clamp(length(flow) * 5.0, 0.0, 1.6) * max(w.flowing, step(0.02, length(flow)));
	vec2 fd = speed > 0.01 ? normalize(flow) : wd;
	mat2 to_flow = mat2(fd.x, -fd.y, fd.y, fd.x); // world xz -> (along, across)
	vec2 q = speed > 0.01 ? to_flow * p * vec2(0.45, 1.0) : p; // streaky along the flow
	vec2 mv = speed > 0.01 ? vec2(-t * (0.6 + speed * 1.8) * 0.45, 0.0) : vec2(0.0);
	// chop: two octaves drifting with the wind, at different speeds
	float ca = 0.05 * mix(chop * mix(0.35, 1.0, s), 0.9 + speed * 0.4, step(0.01, speed))
		* smoothstep(0.0, 0.8, w.shore + 0.3);
	vec3 n1 = fxVNoise2d(q * 1.3 + mv * 1.3 + wd * t * 0.45 * (1.0 - step(0.01, speed)));
	vec3 n2 = fxVNoise2d(mat2(0.8, -0.6, 0.6, 0.8) * q * 2.9 + mv * 2.9 + wd * t * 0.8 + 17.0);
	vec2 g1 = n1.yz * 1.3, g2 = n2.yz * 2.9 * mat2(0.8, -0.6, 0.6, 0.8);
	if (speed > 0.01) {
		// back from (along, across) to world xz, with the along squash
		g1 = (g1 * vec2(0.45, 1.0)) * to_flow;
		g2 = (g2 * vec2(0.45, 1.0)) * to_flow;
	}
	h += vec3(n1.x - 0.5, g1) * ca;
	h += vec3(n2.x - 0.5, g2) * ca * 0.45 * (1.0 - smoothstep(0.15, 0.4, footprint));
	// rain rings, where a pixel resolves them
	if (fxRaining > 0.01) {
		float rf = 1.0 - smoothstep(0.04, 0.15, footprint);
		if (rf > 0.0)
			h.yz += fxWaterRain(p) * fxRaining * rf;
	}
	// ripples: fine, everywhere (ponds too), only where a pixel resolves them
	float rf = 1.0 - smoothstep(0.03, 0.12, footprint);
	if (rf > 0.0) {
		vec3 r1 = fxVNoise2d(p * 7.0 + vec2(t * 0.9, -t * 0.6));
		vec3 r2 = fxVNoise2d(p * 11.0 - vec2(t * 0.7, t * 1.1) + 31.0);
		float ra = 0.006 * (0.6 + chop) * rf;
		h += vec3(0.0, r1.yz * 7.0 + r2.yz * 11.0 * 0.6) * ra;
	}
	return h;
}

// Foam: lapping at the water's edge, breakers running in on open water,
// whitecaps on high crests in rough weather. h: wave height, amp_max: its
// range. Returns coverage 0..1.
float fxWaterFoam(vec2 p, FxWaterInfo w, float h, float amp_max, float contact, vec2 flow)
{
	float t = fxTime;
	float s = fxWaterScale(w);
	float chop = fxWaterChop;
	vec2 drift = vec2(cos(fxWindAngle), sin(fxWindAngle)) * t * 0.15;
	// a bubbly texture: two layers of noise, thresholded
	float tex = fxFbm2(p * 3.1 + drift) * 0.65 + fxFbm2(p * 7.3 - drift * 1.7 + 9.0) * 0.35;
	// lapping: a thin edge that breathes in and out
	float lap_w = 0.3 + 0.15 * sin(t * 1.3 + tex * 3.0) + 0.3 * s;
	float lap = (1.0 - smoothstep(0.0, lap_w, w.shore)) * w.known;
	// breakers: bands parallel to the shore, running in, on open water only
	float band = sin(w.shore * 2.1 + t * 1.8 + tex * 2.5) * 0.5 + 0.5;
	float breaker = smoothstep(0.72, 0.95, band) * (1.0 - smoothstep(0.8, 4.5, w.shore))
		* smoothstep(0.15, 0.6, s) * clamp(chop * 1.2, 0.0, 1.0) * w.known;
	// whitecaps: the tops of the highest crests when it blows
	float crest = smoothstep(0.55, 0.95, amp_max > 1e-4 ? h / amp_max : 0.0)
		* smoothstep(0.8, 1.4, chop) * s;
	// around things standing in the water (the scene depth knows those)
	// running water: streaks of foam carried downstream, more where it's fast
	float speed = clamp(length(flow) * 5.0, 0.0, 1.6) * w.flowing;
	float streak = 0.0;
	if (speed > 0.05) {
		vec2 fd = normalize(flow);
		vec2 q = vec2(dot(p, fd), dot(p, vec2(-fd.y, fd.x)));
		streak = smoothstep(0.55, 0.8, fxFbm2(vec2(q.x * 0.6 - t * (0.6 + speed * 1.8), q.y * 2.4)))
			* smoothstep(0.05, 0.8, speed);
	}
	float cover = max(max(max(lap, breaker), max(crest, contact)), streak);
	// the texture breaks the cover up into bubbles and streaks
	float bubbly = smoothstep(0.45, 0.8, tex + cover * 0.35);
	return clamp(bubbly * min(cover * 1.5, 1.0) * fxFoam * 1.4, 0.0, 1.0);
}
#endif
