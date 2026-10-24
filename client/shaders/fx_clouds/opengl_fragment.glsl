#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
// fx: volumetric clouds, half resolution. Replace the game's cloud boxes
// (Clouds::render skips its mesh) with soft, lit, see-through volumes in the
// same layer: height, thickness, density, colours and drift come from the
// game (Clouds::fxPublish), so set_clouds and the weather still drive them.
// Shapes keep a chunky, blocky layout (64-node cells, clustered like the
// game's noise) with soft, wispy edges (fx_cloud_softness).
// Output: rgb = light scattered towards the eye (linear), a = transmittance.
// The march stops at the scene, so clouds sit behind terrain correctly.
#define depthMap texture0
uniform sampler2D depthMap;

CENTROID_ VARYING_ mediump vec2 varTexCoord;

#define CLOUD_H fxCloud.x
#define CLOUD_T fxCloud.y
#define CLOUD_D fxCloud.z

float fxNoise2c(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(fxHash13(vec3(i, 9.0)), fxHash13(vec3(i + vec2(1, 0), 9.0)), f.x),
	           mix(fxHash13(vec3(i + vec2(0, 1), 9.0)), fxHash13(vec3(i + vec2(1, 1), 9.0)), f.x), f.y);
}

// is this 64-node cell cloud? clustered like the game's fractal noise
float cellFill(vec2 cell)
{
	vec2 q = cell * 0.32;
	float n = fxNoise2c(q) * 0.57 + fxNoise2c(q * 2.0 + 3.1) * 0.29 + fxNoise2c(q * 4.0 + 7.7) * 0.14;
	return smoothstep(CLOUD_D + 0.04, CLOUD_D - 0.04, n);
}

// density at a world position (nodes)
float cloudDensity(vec3 w)
{
	float yf = (w.y - CLOUD_H) / max(CLOUD_T, 0.5);
	if (yf < 0.0 || yf > 1.0)
		return 0.0;
	vec2 c = (w.xz - fxCloudOrigin.xy / FX_BS) / 64.0 - 0.5;
	vec2 i = floor(c), f = fract(c);
	float a = cellFill(i), b = cellFill(i + vec2(1, 0)), cc = cellFill(i + vec2(0, 1)), d = cellFill(i + vec2(1, 1));
	float soft = fxCloudSoftness;
	// sharpen the bilinear blend: soft 0 = blocky, 1 = rounded
	vec2 g = mix(step(0.5, f), smoothstep(0.5 - 0.45 * soft, 0.5 + 0.45 * soft, f), clamp(soft, 0.0, 1.0));
	float field = mix(mix(a, b, g.x), mix(cc, d, g.x), g.y);
	if (field <= 0.01)
		return 0.0;
	// rounded tops and bottoms
	float prof = smoothstep(0.0, 0.25 * soft + 0.02, yf) * smoothstep(1.0, 1.0 - (0.45 * soft + 0.02), yf);
	// wispy edges: erode by 3D noise, more where the field is thin
	float n = fxNoise3(w * 0.045 + vec3(fxTime * 0.02, 0.0, 0.0)) * 0.65
		+ fxNoise3(w * 0.13) * 0.35;
	float dens = field * prof - (1.0 - field * prof) * n * 0.6 * soft - n * 0.1 * soft;
	return clamp(dens, 0.0, 1.0);
}

void main(void)
{
	if (fxCloud.w < 0.5 || CLOUD_D <= 0.0) {
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec2 uv = varTexCoord.st;
	float d = texture2D(depthMap, uv).r;
	vec3 eyeS = fxEyePos;
	vec3 dir = fxSafeNormalize(fxScenePos(uv, 0.5) - eyeS);
	float scene = d >= 0.99999 ? 1e9 : length(fxScenePos(uv, d) - eyeS) / FX_BS;
	// world position of the eye, in nodes
	vec3 eye = (eyeS + vec3(fxCloudOrigin.z, fxCameraOffsetY, fxCloudOrigin.w)) / FX_BS;
	// the slab the clouds live in
	float y0 = CLOUD_H, y1 = CLOUD_H + max(CLOUD_T, 0.5);
	float t0, t1;
	if (abs(dir.y) < 1e-4) {
		if (eye.y < y0 || eye.y > y1) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
		t0 = 0.0; t1 = 1e9;
	} else {
		float ta = (y0 - eye.y) / dir.y, tb = (y1 - eye.y) / dir.y;
		t0 = max(min(ta, tb), 0.0);
		t1 = max(ta, tb);
	}
	const float MAX_DIST = 1100.0;
	t1 = min(min(t1, scene), MAX_DIST);
	if (t1 <= t0) {
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	const int STEPS = 28;
	float seg = t1 - t0;
	float dt = seg / float(STEPS);
	float jitter = fxIGN(gl_FragCoord.xy + fxFrame * 3.17);
	vec3 L = fxIsDay > 0.5 ? fxSunDir : vec3(0.0, 1.0, 0.0);
	float mu = dot(dir, L);
	// a little forward scattering: silver edges towards the sun
	float phase = 0.6 + 0.8 * pow(max(mu, 0.0), 6.0);
	vec3 sun_c = fxCloudColor.rgb;
	vec3 amb_c = mix(fxCloudShadow.rgb * fxCloudColor.rgb, fxToLinear(fxFogColor), 0.3);
	float sigma_k = 0.22 * fxCloudOpacity; // extinction per node at density 1
	// lightning: the cloud around the strike lights up from inside
	float bolt = fxFlash * fxLightningFlash;
	vec2 strike = fxStrikePos + fxCloudOrigin.zw / FX_BS; // world nodes, like w
	vec3 acc = vec3(0.0);
	float T = 1.0;
	for (int i = 0; i < STEPS; i++) {
		float t = t0 + (float(i) + jitter) * dt;
		vec3 w = eye + dir * t;
		float den = cloudDensity(w);
		if (den <= 0.001)
			continue;
		// light: two taps towards the sun
		float od = cloudDensity(w + L * 6.0) * 6.0 + cloudDensity(w + L * 18.0) * 12.0;
		float sunlit = exp(-od * sigma_k * 0.9);
		float yf = (w.y - y0) / max(y1 - y0, 0.5);
		vec3 lum = sun_c * sunlit * phase * 1.3 + amb_c * mix(0.3, 0.8, yf);
		if (bolt > 0.001) {
			float ds = length(w.xz - strike);
			lum += vec3(0.75, 0.82, 1.0) * bolt * 9.0 * exp(-ds / 70.0) * (1.0 - yf * 0.5);
		}
		float s = den * sigma_k;
		float a = 1.0 - exp(-s * dt);
		acc += T * a * lum;
		T *= 1.0 - a;
		if (T < 0.02)
			break;
	}
	// fade with distance into the haze (the game's clouds fade in the fog)
	float fade = 1.0 - smoothstep(MAX_DIST * 0.55, MAX_DIST, t0);
	gl_FragColor = vec4(acc * fade, mix(1.0, T, fade));
}
