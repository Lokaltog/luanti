// fx: every fx uniform, as three blocks uploaded once per frame.
// C++ side: src/client/fx/params.def (FxParams, generated), renderer.h
// (FrameBlock, LightsBlock: keep the std140 layouts in sync).
// Self-guarded, so any fx file can include it at top level.
#ifdef FX_ENABLE
#include "fx_generated/params.glsl"

layout(std140) uniform FxFrame {
	mat4 fxViewProj;          // scene space (relative to cameraOffset) -> clip, jittered
	mat4 fxInvViewProj;
	mat4 fxPrevViewProj;      // previous frame, unjittered, its own scene space
	vec4 fxEyeTime;
	vec4 fxSun;
	vec4 fxNearFarJitter;
	vec4 fxVolume;
	vec4 fxMisc;
	vec4 fxOffsetDeltaProj;
	vec4 fxWater;
};
#define fxEyePos fxEyeTime.xyz          // camera in scene space
#define fxTime fxEyeTime.w              // seconds, wraps hourly
#define fxSunDir fxSun.xyz              // towards the sun (or the moon at night)
#define fxSunStrength fxSun.w           // 0 below the horizon .. 1
#define fxNearFar fxNearFarJitter.xy
#define fxJitter fxNearFarJitter.zw     // TAA jitter in NDC (0 without TAA)
#define fxVolumeOrigin fxVolume.xyz     // scene-space centre of light voxel (0,0,0)
#define fxVolumeValid fxVolume.w
#define fxUnderwater fxMisc.x
#define fxSceneValid fxMisc.y           // the scene copy exists this frame
#define fxTaaReset fxMisc.z
#define fxFrame fxMisc.w
#define fxOffsetDelta fxOffsetDeltaProj.xyz // camera offset change (scene units)
#define fxProjScale fxOffsetDeltaProj.w     // pixels per scene unit at depth 1
#define fxWaterLevel fxWater.x              // scene y of the water surface (when underwater)

layout(std140) uniform FxLights {
	vec4 fxLightPosRadius[FX_LIGHT_MAX];  // xyz scene space, w radius
	vec4 fxLightColorSeed[FX_LIGHT_MAX];  // rgb chroma, w: int = flicker*100, fract = seed
	ivec4 fxLightInfo;                    // count, held-light index
};
#define fxLightCount fxLightInfo.x
#define fxHeldLightIndex fxLightInfo.y

const float FX_BS = 10.0;
#endif
