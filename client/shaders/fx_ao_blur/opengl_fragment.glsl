#include "fx_common/uniforms.glsl"
// fx: 5x5 bilateral blur of the half-resolution AO (R) and contact shadow
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
	ivec2 size = textureSize(aoTex, 0);
	ivec2 p = clamp(ivec2(varTexCoord.st * vec2(size)), ivec2(0), size - 1);
	vec4 c = texelFetch(aoTex, p, 0);
	float d0 = unpackDist(c.ba);
	if (d0 >= 0.9999) { // sky
		gl_FragColor = c;
		return;
	}
	// whole texels (texelFetch): no dependence on the sampler's filter
	vec2 acc = vec2(0.0);
	float wsum = 0.0;
	for (int y = -2; y <= 2; y++)
	for (int x = -2; x <= 2; x++) {
		vec4 s = texelFetch(aoTex, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0);
		float rel = abs(unpackDist(s.ba) - d0) / max(d0, 1e-4);
		float w = exp(-rel * rel * 400.0) // ~5% depth difference: weight 0.37
			* exp(-float(x * x + y * y) * 0.18); // spatial falloff
		acc += s.rg * w;
		wsum += w;
	}
	gl_FragColor = vec4(acc / max(wsum, 1e-4), c.ba);
}
