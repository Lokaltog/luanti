#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
// fx: screen-space bounce light, half resolution (wave 3).
// For each pixel, gather the lit colour of nearby surfaces on screen that
// face it: a red wall tints the floor in front of it, lava lights the cave
// ceiling orange. One bounce, from what is on screen only (no off-screen
// or hidden emitters); rotated per pixel, smoothed in the composite.
// Output: rgb = incoming bounce light (linear), a = 1.
#define colorMap texture0
#define depthMap texture1
uniform sampler2D colorMap;
uniform sampler2D depthMap;

CENTROID_ VARYING_ mediump vec2 varTexCoord;

vec3 posAt(vec2 uv)
{
	return fxScenePos(uv, texture2D(depthMap, uv).r);
}

void main(void)
{
	vec2 uv = varTexCoord.st;
	float d = texture2D(depthMap, uv).r;
	if (d >= 0.99999) {
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec2 px = 1.0 / vec2(textureSize(depthMap, 0));
	vec3 P = posAt(uv);
	float dist = length(P - fxEyePos);
	if (dist < 0.75 * FX_BS) { // the wielded item
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec3 N = fxSafeNormalize(cross(posAt(uv + vec2(px.x, 0.0)) - P, posAt(uv + vec2(0.0, px.y)) - P));
	if (dot(N, fxEyePos - P) < 0.0)
		N = -N;
	// gather radius: ~2.5 nodes in the world, in screen space
	float r_px = clamp(2.5 * FX_BS * fxProjScale / max(dist, 1.0), 4.0, 160.0);
	float rot = fxIGN(gl_FragCoord.xy) * 6.2831853;
	vec3 acc = vec3(0.0);
	float wsum = 0.0;
	const int N_SAMPLES = 8;
	for (int i = 0; i < N_SAMPLES; i++) {
		float t = (float(i) + 0.5) / float(N_SAMPLES);
		float a = rot + float(i) * 2.39996;
		vec2 o = vec2(cos(a), sin(a)) * sqrt(t) * r_px * px;
		vec2 suv = uv + o;
		if (suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0)
			continue;
		float sd = texture2D(depthMap, suv).r;
		if (sd >= 0.99999)
			continue;
		vec3 S = fxScenePos(suv, sd);
		vec3 v = S - P;
		float l2 = dot(v, v);
		float l = sqrt(l2);
		vec3 dir = v / max(l, 1e-3);
		float cos_p = dot(N, dir);
		if (cos_p <= 0.05 || l > 4.0 * FX_BS)
			continue;
		// light leaving the sample towards P: its colour, falling off with
		// distance (in nodes)
		vec3 c = fxToLinear(texture2D(colorMap, suv).rgb);
		float w = cos_p / (1.0 + l2 / (FX_BS * FX_BS));
		acc += c * w;
		wsum += 1.0;
	}
	gl_FragColor = vec4(acc / float(N_SAMPLES) * 2.0, 1.0);
}
