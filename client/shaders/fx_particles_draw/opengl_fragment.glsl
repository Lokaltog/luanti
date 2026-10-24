// fx: GPU particle draw (see the vertex shader). Premultiplied alpha into
// the display-encoded scene buffer: glowing kinds write alpha 0 and add,
// leaves and snow cover what is behind. Soft particles fade into the opaque
// scene where the scene copy's depth is available.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#define fxpLeafTiles texture4
#define fxpSceneDepth texture5
uniform sampler2DArray fxpLeafTiles;
uniform sampler2D fxpSceneDepth;
#include "fx_common/particles.glsl"

VARYING_ vec2 vUV;
VARYING_ float vDepth;
flat VARYING_ vec4 vColor;
flat VARYING_ vec4 vInfo;
flat VARYING_ vec3 vTint;

void main(void)
{
	float kind = vInfo.x;
	vec3 col = vColor.rgb;
	float a = vColor.a;
	float r2 = dot(vUV, vUV);
	if (fxpIs(kind, FXP_SPARK) || fxpIs(kind, FXP_RAIN) || fxpIs(kind, FXP_DRIP)) {
		float across = 1.0 - abs(vUV.x);
		a *= across * across * smoothstep(-1.0, 0.1, vUV.y);
		// a bright head on sparks
		if (fxpIs(kind, FXP_SPARK))
			col *= 0.6 + 0.6 * smoothstep(0.2, 0.9, vUV.y);
	} else if (fxpIs(kind, FXP_LEAF)) {
		// one leaf: an almond shape (pointed at both ends, a little curved),
		// coloured from a small patch of the tree's tile, with a midrib
		float layer = floor(vInfo.y), seed = fract(vInfo.y) / 0.9;
		vec2 q = vUV;
		q.x += q.y * q.y * (seed - 0.5) * 0.5; // a slight curl
		float half_w = 0.55 * (1.0 - q.y * q.y);
		if (abs(q.x) > half_w || abs(q.y) > 0.98)
			discard;
		// four taps in a 4x4-texel patch picked by the seed: the leaf's colour
		vec2 base = vec2(fract(seed * 7.13), fract(seed * 3.71)) * 0.75 + 2.0 / 16.0;
		vec3 sum = vec3(0.0);
		float n = 0.0;
		for (int i = 0; i < 4; i++) {
			vec2 o = vec2(float(i & 1), float(i >> 1)) * (2.0 / 16.0);
			vec4 t = texture(fxpLeafTiles, vec3(fract(base + o), layer));
			sum += t.rgb * step(0.5, t.a);
			n += step(0.5, t.a);
		}
		vec3 leaf_c = n > 0.0 ? sum / n : vec3(0.3, 0.5, 0.2);
		// midrib and edge: a little darker
		float shade = 0.85 + 0.15 * smoothstep(0.0, 0.12, abs(q.x))
			- 0.2 * smoothstep(half_w * 0.6, half_w, abs(q.x));
		col *= fxToLinear(leaf_c) * vTint * shade;
	} else if (fxpIs(kind, FXP_SPLASH)) {
		float r = sqrt(r2);
		a *= smoothstep(0.45, 0.8, r) * (1.0 - smoothstep(0.85, 1.0, r));
	} else if (fxpIs(kind, FXP_FIREFLY)) {
		// a hot core in a soft halo
		float core = exp(-r2 * 45.0), halo = exp(-r2 * 5.0) * 0.12;
		col *= core * 1.2 + halo;
		a *= step(r2, 1.0);
	} else {
		a *= 1.0 - smoothstep(0.25, 1.0, sqrt(r2));
	}
	// soft particles: fade where they meet the opaque scene
	if (vInfo.w > 0.0 && fxSceneValid > 0.5) {
		float d = texelFetch(fxpSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
		a *= clamp((fxLinearDepth(d) - vDepth) / (vInfo.w * FX_BS), 0.0, 1.0);
	}
	if (a <= 0.002)
		discard;
	gl_FragColor = vec4(fxToDisplay(col) * a, a * vInfo.z);
}
