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
		vec4 t = texture(fxpLeafTiles, vec3(vUV * 0.5 + 0.5, vInfo.y));
		if (t.a < 0.5)
			discard;
		col *= fxToLinear(t.rgb) * vTint;
	} else if (fxpIs(kind, FXP_SPLASH)) {
		float r = sqrt(r2);
		a *= smoothstep(0.45, 0.8, r) * (1.0 - smoothstep(0.85, 1.0, r));
	} else if (fxpIs(kind, FXP_FIREFLY)) {
		// a hot core in a soft halo
		float core = exp(-r2 * 45.0), halo = exp(-r2 * 5.0) * 0.25;
		col *= core * 3.0 + halo;
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
