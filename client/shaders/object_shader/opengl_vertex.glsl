uniform mat4 mWorld;
uniform vec3 dayLight;
uniform float animationTimer;
uniform lowp vec4 materialColor;

VARYING_ vec3 vNormal;
VARYING_ vec3 worldPosition;
VARYING_ lowp vec4 varColor;
CENTROID_ VARYING_ mediump vec2 varTexCoord;
#ifdef USE_ARRAY_TEXTURE
flat VARYING_ uint varTexLayer;
#endif

#ifdef ENABLE_DYNAMIC_SHADOWS
	// shadow uniforms
	uniform vec3 v_LightDirection;
	uniform float f_textureresolution;
	uniform mat4 m_ShadowViewProj;
	uniform float f_shadowfar;
	uniform float f_shadow_strength;
	uniform float f_timeofday;
	uniform vec4 CameraPos;

	VARYING_ float cosLight;
	VARYING_ float adj_shadow_strength;
	VARYING_ float f_normal_length;
	VARYING_ vec3 shadow_position;
	VARYING_ float perspective_factor;
#endif

VARYING_ highp vec3 eyeVec;
VARYING_ float nightRatio;
// Color of the light emitted by the light sources.
const vec3 artificialLight = vec3(1.04, 1.04, 1.04);

#ifdef FX_ENABLE
uniform float fxAmbientFloor;
#endif

// fx: same light list and tint as nodes_shader/opengl_vertex.glsl (keep in sync)
#if defined(FX_ENABLE) && defined(FX_COLORED_LIGHT)
// fx: nearest light sources this frame, positions relative to cameraOffset (BS units)
uniform int fxLightCount;
uniform vec4 fxLightPosRadius[FX_LIGHT_MAX]; // xyz = position, w = radius
uniform vec4 fxLightColorSeed[FX_LIGHT_MAX]; // rgb = chroma, a = floor: flicker amp*100, fract: seed
uniform float fxLightSaturation;
VARYING_ vec3 fxChroma; // for debug view 1

// Hue (and flicker) of the artificial light at p. Intensity is NOT changed here:
// the engine's flood-filled scalar light stays authoritative, so colour may
// bleed through walls but brightness never does.
vec3 fxArtificialLight(vec3 p, float seconds)
{
	vec3 acc = vec3(0.0);
	float wsum = 0.0;
	float flick = 0.0;
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
		float n = sin(seconds * 7.3 + seed) * 0.5
			+ sin(seconds * 13.7 + seed * 1.7) * 0.3
			+ sin(seconds * 23.1 + seed * 2.3) * 0.2;
		acc += w * cs.rgb;
		flick += w * amp * n;
		wsum += w;
	}
	if (wsum <= 1e-5) {
		fxChroma = vec3(1.0);
		return artificialLight;
	}
	vec3 chroma = acc / wsum;
	fxChroma = chroma;
	// keep the luminance of upstream's white light so exposure is unchanged
	float lum = dot(chroma, vec3(0.2126, 0.7152, 0.0722));
	chroma = min(chroma * (1.04 / max(lum, 0.05)), vec3(2.4));
	return mix(artificialLight, chroma, fxLightSaturation) * (1.0 + flick / wsum);
}
#endif
VARYING_ float vIDiff;

#ifdef USE_SKINNING
layout (std140) uniform JointMatrices {
	mat4 joints[MAX_JOINTS];
};
#endif

#ifdef ENABLE_DYNAMIC_SHADOWS

uniform float xyPerspectiveBias0;
uniform float xyPerspectiveBias1;
uniform float zPerspectiveBias;

vec4 getRelativePosition(in vec4 position)
{
	vec2 l = position.xy - CameraPos.xy;
	vec2 s = l / abs(l);
	s = (1.0 - s * CameraPos.xy);
	l /= s;
	return vec4(l, s);
}

float getPerspectiveFactor(in vec4 relativePosition)
{
	float pDistance = length(relativePosition.xy);
	float pFactor = pDistance * xyPerspectiveBias0 + xyPerspectiveBias1;
	return pFactor;
}

vec4 applyPerspectiveDistortion(in vec4 position)
{
	vec4 l = getRelativePosition(position);
	float pFactor = getPerspectiveFactor(l);
	l.xy /= pFactor;
	position.xy = l.xy * l.zw + CameraPos.xy;
	position.z *= zPerspectiveBias;
	return position;
}

#if __VERSION__ >= 130
#define mtsmoothstep smoothstep
#else
float mtsmoothstep(in float edge0, in float edge1, in float x)
{
	float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
#endif

#endif


float directional_ambient(vec3 normal)
{
	vec3 v = normal * normal;

	if (normal.y < 0.0)
		return dot(v, vec3(0.670820, 0.447213, 0.836660));

	return dot(v, vec3(0.670820, 1.000000, 0.836660));
}

void main(void)
{
#ifdef USE_SKINNING
	uvec4 jids = inVertexJointIDs;
	vec4 skinPos = inVertexPosition;
	vec3 skinNormal = inVertexNormal;
	// Alternatively: Introduce neutral bone at index 0 with identity matrix?
	if (inVertexWeights != vec4(0.0)) {
		// Note that this deals correctly with a disabled vertex attribute.
		mat4 mSkin =
				inVertexWeights.x * joints[jids.x] +
				inVertexWeights.y * joints[jids.y] +
				inVertexWeights.z * joints[jids.z] +
				inVertexWeights.w * joints[jids.w];
		skinPos = vec4((mSkin * vec4(inVertexPosition.xyz, 1.0)).xyz, 1.0);
		skinNormal = (mSkin * vec4(inVertexNormal, 0.0)).xyz;
	}
#else
	vec4 skinPos = inVertexPosition;
	vec3 skinNormal = inVertexNormal;
#endif

#ifdef USE_ARRAY_TEXTURE
	varTexLayer = inVertexAux;
#endif
	varTexCoord = (mTexture * vec4(inTexCoord0.xy, 1.0, 1.0)).st;

	gl_Position = mWorldViewProj * skinPos;

	vNormal = (mWorld * vec4(skinNormal, 0.0)).xyz;
	worldPosition = (mWorld * skinPos).xyz;
	eyeVec = -(mWorldView * skinPos).xyz;

#if (MATERIAL_TYPE == TILE_MATERIAL_PLAIN) || (MATERIAL_TYPE == TILE_MATERIAL_PLAIN_ALPHA)
	vIDiff = 1.0;
#else
	// This is intentional comparison with zero without any margin.
	// If normal is not equal to zero exactly, then we assume it's a valid, just not normalized vector
	vIDiff = length(vNormal) == 0.0
		? 1.0
		: directional_ambient(normalize(vNormal));
#endif

	vec4 color = inVertexColor;

	color *= materialColor;

	// The alpha gives the ratio of sunlight in the incoming light.
	nightRatio = 1.0 - color.a;
#if defined(FX_ENABLE) && defined(FX_COLORED_LIGHT)
	vec3 fxLight = fxArtificialLight(worldPosition, animationTimer * 100.0);
	color.rgb = color.rgb * (color.a * dayLight.rgb +
		nightRatio * fxLight) * 2.0;
#else
	color.rgb = color.rgb * (color.a * dayLight.rgb +
		nightRatio * artificialLight.rgb) * 2.0;
#endif
	color.a = 1.0;
#ifdef FX_ENABLE
	color.rgb += fxAmbientFloor * (1.0 - clamp(dot(color.rgb, vec3(0.333)) * 6.0, 0.0, 1.0));
#endif

	// Emphase blue a bit in darker places
	// See C++ implementation in mapblock_mesh.cpp final_color_blend()
	float brightness = (color.r + color.g + color.b) / 3.0;
	color.b += max(0.0, 0.021 - abs(0.2 * brightness - 0.021) +
		0.07 * brightness);

	varColor = clamp(color, 0.0, 1.0);


#ifdef ENABLE_DYNAMIC_SHADOWS
	if (f_shadow_strength > 0.0) {
		vec3 nNormal = normalize(vNormal);
		f_normal_length = length(vNormal);

		/* normalOffsetScale is in world coordinates (1/10th of a meter)
		   z_bias is in light space coordinates */
		float normalOffsetScale, z_bias;
		float pFactor = getPerspectiveFactor(getRelativePosition(m_ShadowViewProj * mWorld * inVertexPosition));
		if (f_normal_length > 0.0) {
			nNormal = normalize(vNormal);
			cosLight = max(1e-5, dot(nNormal, -v_LightDirection));
			float sinLight = pow(1.0 - pow(cosLight, 2.0), 0.5);
			normalOffsetScale = 0.1 * pFactor * pFactor * sinLight * min(f_shadowfar, 500.0) /
					xyPerspectiveBias1 / f_textureresolution;
			z_bias = 1e3 * sinLight / cosLight * (0.5 + f_textureresolution / 1024.0);
		}
		else {
			nNormal = vec3(0.0);
			cosLight = clamp(dot(v_LightDirection, normalize(vec3(v_LightDirection.x, 0.0, v_LightDirection.z))), 1e-2, 1.0);
			float sinLight = pow(1.0 - pow(cosLight, 2.0), 0.5);
			normalOffsetScale = 0.0;
			z_bias = 3.6e3 * sinLight / cosLight;
		}
		z_bias *= pFactor * pFactor / f_textureresolution / f_shadowfar;

		shadow_position = applyPerspectiveDistortion(m_ShadowViewProj * mWorld * (inVertexPosition + vec4(normalOffsetScale * nNormal, 0.0))).xyz;
		shadow_position.z -= z_bias;
		perspective_factor = pFactor;

		if (f_timeofday < 0.2) {
			adj_shadow_strength = f_shadow_strength * 0.5 *
				(1.0 - mtsmoothstep(0.18, 0.2, f_timeofday));
		} else if (f_timeofday >= 0.8) {
			adj_shadow_strength = f_shadow_strength * 0.5 *
				mtsmoothstep(0.8, 0.83, f_timeofday);
		} else {
			adj_shadow_strength = f_shadow_strength *
				mtsmoothstep(0.20, 0.25, f_timeofday) *
				(1.0 - mtsmoothstep(0.7, 0.8, f_timeofday));
		}
	}
#endif
}
