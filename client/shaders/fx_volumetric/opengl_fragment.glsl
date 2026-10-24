#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
// fx: volumetric light, half resolution (wave 3). Marches the view ray
// through the near air: artificial light from the RGB light volume glows
// around torches, lava and lamps, coloured and leak-free (the volume only
// holds light where light can travel). Output: rgb = in-scattered light
// (linear), added in the composite, which also does the height fog
// analytically.
#define depthMap texture0
#define fxVolTex texture1
uniform sampler2D depthMap;
uniform sampler2D fxVolTex; // the light volume atlas (see light_volume.h)

CENTROID_ VARYING_ mediump vec2 varTexCoord;

const vec3 VOL_SIZE = vec3(float(FX_VOLUME_SX), float(FX_VOLUME_SY), float(FX_VOLUME_SZ));
const float VOL_COLS = float(FX_VOLUME_COLS);
// the texture stacks two atlases (levels, then the sky tint): twice as tall
const vec2 VOL_ATLAS = vec2(VOL_SIZE.x * VOL_COLS, 2.0 * VOL_SIZE.z * ceil(VOL_SIZE.y / VOL_COLS));

vec3 volLight(vec3 p)
{
	vec3 v = (p - fxVolumeOrigin) / FX_BS;
	if (fxVolumeValid < 0.5 || any(lessThan(v, vec3(0.0))) || any(greaterThan(v, VOL_SIZE - 1.0)))
		return vec3(0.0);
	float y0 = floor(v.y);
	vec2 xz = v.xz + 0.5;
	vec2 t = (vec2(mod(y0, VOL_COLS), floor(y0 / VOL_COLS)) * VOL_SIZE.xz + xz) / VOL_ATLAS;
	vec3 l = textureLod(fxVolTex, t, 0.0).rgb; // levels 0..1 = 0..15
	// the air glows near sources, not across a whole lit room: a steep
	// curve keeps the glow to the last few nodes before the light
	vec3 l2 = l * l;
	return l2 * l2 * l2;
}

void main(void)
{
	if (fxVolumetric <= 0.0 || fxVolumeValid < 0.5) {
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec2 uv = varTexCoord.st;
	float d = texture2D(depthMap, uv).r;
	vec3 eye = fxEyePos;
	vec3 target = fxScenePos(uv, d >= 0.99999 ? 0.9999 : d);
	vec3 ray = target - eye;
	float dist = length(ray);
	vec3 dir = ray / max(dist, 1e-4);
	// light glows in the near air only (the volume is the 96-node window)
	float len = min(dist, 48.0 * FX_BS);
	const int STEPS = 12;
	float jitter = fxIGN(gl_FragCoord.xy);
	float dt = len / float(STEPS);
	vec3 acc = vec3(0.0);
	for (int i = 0; i < STEPS; i++) {
		vec3 p = eye + dir * ((float(i) + jitter) * dt);
		acc += volLight(p);
	}
	// thin haze: in-scatter grows with the length through lit air
	gl_FragColor = vec4(acc * (dt / FX_BS) * fxVolumetric * 0.02, 1.0);
}
