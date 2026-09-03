#ifndef OLO_GPU_SCENE_LIGHTS_GLSL
#define OLO_GPU_SCENE_LIGHTS_GLSL

// The canonical light table. Aliases SSBO_FPLUS_POINT_LIGHTS, so a consumer
// cannot also include ForwardPlusCommon.glsl.
// See include/GPUScene.glsl for the record contract and the per-kind slot map.
#include "GPUScene.glsl"

layout(std430, binding = 9) readonly buffer OloGPUSceneLights
{
    GPUSceneLight g_GPUSceneLights[];
};

#endif // OLO_GPU_SCENE_LIGHTS_GLSL
