#ifndef OLO_GPU_SCENE_ENVIRONMENTS_GLSL
#define OLO_GPU_SCENE_ENVIRONMENTS_GLSL

// The canonical environment table. Aliases SSBO_FPLUS_SPOT_LIGHTS.
// See include/GPUScene.glsl for the record contract and the per-kind slot map.
#include "GPUScene.glsl"

layout(std430, binding = 10) readonly buffer OloGPUSceneEnvironments
{
    GPUSceneEnvironment g_GPUSceneEnvironments[];
};

#endif // OLO_GPU_SCENE_ENVIRONMENTS_GLSL
