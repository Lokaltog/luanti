// fx: every fx uniform, as three blocks uploaded once per frame.
// C++ side: src/client/fx/params.def (FxParams, generated), renderer.h
// (FrameBlock, LightsBlock: keep the std140 layouts in sync).
// Self-guarded, so any fx file can include it at top level.
#ifdef FX_ENABLE
#include "fx_generated/params.glsl"

// Hard toggles (tuner checkboxes, fx_use_*): a disabled effect's strength
// becomes the constant 0, so its branch folds away at compile time and the
// shader does none of its work.
#ifndef FX_ON_USE_PARALLAX
#undef fxParallaxDepth
#define fxParallaxDepth 0.0
#endif
#ifndef FX_ON_USE_SPECULAR
#undef fxSpecular
#define fxSpecular 0.0
#endif
#ifndef FX_ON_USE_GLINTS
#undef fxGlints
#define fxGlints 0.0
#endif
#ifndef FX_ON_USE_CONTACT_SHADOWS
#undef fxContactShadows
#define fxContactShadows 0.0
#endif
#ifndef FX_ON_USE_CAUSTICS
#undef fxCaustics
#define fxCaustics 0.0
#endif
#ifndef FX_ON_USE_SHAFTS
#undef fxUnderwaterShafts
#define fxUnderwaterShafts 0.0
#endif
#ifndef FX_ON_USE_HEAT_HAZE
#undef fxHeatHaze
#define fxHeatHaze 0.0
#endif
#ifndef FX_ON_USE_FOG_BLUR
#undef fxFogBlur
#define fxFogBlur 0.0
#endif
#ifndef FX_ON_USE_LENS_DIRT
#undef fxLensDirt
#define fxLensDirt 0.0
#endif
#ifndef FX_ON_USE_END_SKY
#undef fxEndSky
#define fxEndSky 0.0
#endif
#ifndef FX_ON_USE_PORTAL_WARP
#undef fxPortalWarp
#define fxPortalWarp 0.0
#endif
#ifndef FX_ON_USE_LIGHTNING
#undef fxLightningFlash
#define fxLightningFlash 0.0
#endif
#ifndef FX_ON_USE_WET
#undef fxWetness
#define fxWetness 0.0
#endif
#ifndef FX_ON_USE_SNOW
#undef fxSnow
#define fxSnow 0.0
#endif
#ifndef FX_ON_USE_FOAM
#undef fxFoam
#define fxFoam 0.0
#endif
#ifndef FX_ON_USE_PBR_SKY
#undef fxSkyPbr
#define fxSkyPbr 0.0
#endif
#ifndef FX_ON_USE_AURORA
#undef fxAurora
#define fxAurora 0.0
#endif
#ifndef FX_ON_USE_DROPLETS
#undef fxDropletsFx
#define fxDropletsFx 0.0
#endif
#ifndef FX_ON_USE_TRANSLUCENCY
#undef fxTranslucency
#define fxTranslucency 0.0
#endif
#ifndef FX_ON_USE_DAMAGE_FX
#undef fxDamageFx
#define fxDamageFx 0.0
#endif

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
	vec4 fxEvents;
	vec4 fxWorld;
	vec4 fxWeather;
	vec4 fxFog;
	vec4 fxWaterStyle;
	vec4 fxSkyInfo;
	vec4 fxCloud;
	vec4 fxCloudOrigin;
	vec4 fxCloudColor;
	vec4 fxCloudShadow;
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
#define fxFlash fxEvents.x                  // lightning, 0..1
#define fxDamage fxEvents.y                 // just hurt, 0..1 decaying
#define fxLowHealth fxEvents.z              // 0..1
#define fxPortal fxEvents.w                 // standing in a portal, 0..1 ramping
#define fxNether fxWorld.x                  // in the Nether, 0..1 smoothed
#define fxEnd fxWorld.y                     // in the End, 0..1 smoothed
#define fxWet fxWorld.z                     // rained on lately, 0..1
#define fxStorm fxWorld.w                   // thunderstorm, 0..1
#define fxSnowCover fxWeather.x             // snow settled on top faces, 0..1
#define fxRaining fxWeather.y               // raining right now, 0..1
#define fxWindStrength fxWeather.z          // ~0.5 calm .. 1.8 storm
#define fxWindAngle fxWeather.w             // radians
#define fxFogColor fxFog.rgb                // the engine's fog colour, display-encoded
#define fxCameraOffsetY fxFog.w             // scene space -> world: + this on y
#define fxWaterTint fxWaterStyle.rgb        // water scatter colour by biome, linear
#define fxWaterChop fxWaterStyle.w          // wave height: calm 0.2 .. storm 1.6
#define fxCold fxSkyInfo.x                  // cold biome, 0..1 (aurora)
#define fxDroplets fxSkyInfo.y              // water on the lens after surfacing, 1 -> 0
#define fxOverworld fxSkyInfo.z             // 1 in the Overworld (sky effects)
#define fxIsDay fxSkyInfo.w                 // 1: fxSunDir is the sun, 0: the moon

layout(std140) uniform FxLights {
	vec4 fxLightPosRadius[FX_LIGHT_MAX];  // xyz scene space, w radius
	vec4 fxLightColorSeed[FX_LIGHT_MAX];  // rgb chroma, w: int = flicker*100, fract = seed
	ivec4 fxLightInfo;                    // count, held-light index
};
#define fxLightCount fxLightInfo.x
#define fxHeldLightIndex fxLightInfo.y
#define fxHeldLevelMax fxLightInfo.z            // held light's level (0: none)

const float FX_BS = 10.0;
#endif
