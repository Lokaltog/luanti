// fx: GPU particle spawn. One GL point per new particle, drawn into the
// slot's texel of every state target (src/client/fx/particles.cpp, push()).
//   Pos = position (nodes, relative to the anchor), Normal = velocity,
//   Color = tint, TCoords = (slot, kind), Tangent = (life, seed, parameter),
//   Binormal = (leaf layer, size, glow)
#include "fx_common/uniforms.glsl"
#include "fx_common/particles.glsl"

flat VARYING_ vec4 vS0;
flat VARYING_ vec4 vS1;
flat VARYING_ vec4 vS2;
flat VARYING_ vec4 vS3;

void main(void)
{
	ivec2 t = fxpTexel(inTexCoord0.x);
	gl_Position = vec4((vec2(t) + 0.5) / fxpStateSize * 2.0 - 1.0, 0.0, 1.0);
	vS0 = vec4(inVertexPosition.xyz, 0.0);
	vS1 = vec4(inVertexNormal, inTexCoord0.y);
	vS2 = vec4(inVertexTangent.xyz, -1.0); // rest age < 0: flying
	vec3 c = floor(inVertexColor.rgb * 255.0 + 0.5);
	vS3 = vec4(c.r * 65536.0 + c.g * 256.0 + c.b, inVertexBinormal.xyz);
}
