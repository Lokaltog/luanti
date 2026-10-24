#include "fx_common/uniforms.glsl"
// fx: 4x4 bilateral blur of the half-resolution AO (R) and contact shadow
// (G). Neighbours count in proportion to how close their view distance (B/A,
// packed by fx_ao) is to the centre's, so occlusion does not bleed across
// depth edges. Without TAA this is the only denoise the AO gets.
#define aoTex texture0
uniform sampler2D aoTex;

CENTROID_ VARYING_ mediump vec2 varTexCoord;

float unpackDist(vec2 ba)
{
	return (ba.x * 255.0 + ba.y) / 255.0;
}

void main(void)
{
	vec2 uv = varTexCoord.st;
	vec2 px = 1.0 / vec2(textureSize(aoTex, 0));
	vec4 c = texture2D(aoTex, uv);
	float d0 = unpackDist(c.ba);
	if (d0 >= 0.9999) { // sky
		gl_FragColor = c;
		return;
	}
	vec2 acc = vec2(0.0);
	float wsum = 0.0;
	for (int y = -2; y <= 1; y++)
	for (int x = -2; x <= 1; x++) {
		vec2 o = (vec2(float(x), float(y)) + 0.5) * px;
		vec4 s = texture2D(aoTex, uv + o);
		float rel = abs(unpackDist(s.ba) - d0) / max(d0, 1e-4);
		float w = exp(-rel * rel * 400.0); // ~5% depth difference: weight 0.37
		acc += s.rg * w;
		wsum += w;
	}
	gl_FragColor = vec4(acc / max(wsum, 1e-4), c.ba);
}
