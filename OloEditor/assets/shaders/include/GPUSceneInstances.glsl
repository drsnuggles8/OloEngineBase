#ifndef OLO_GPU_SCENE_INSTANCES_GLSL
#define OLO_GPU_SCENE_INSTANCES_GLSL

// The canonical instance table. A shader that takes this may not also include
// InstanceBlock*.glsl, which declares the per-draw InstanceData stream at the
// same slot.
// See include/GPUScene.glsl for the record contract and the per-kind slot map.
#include "GPUScene.glsl"

layout(std430, binding = 15) readonly buffer OloGPUSceneInstances
{
    GPUSceneInstance g_GPUSceneInstances[];
};

#endif // OLO_GPU_SCENE_INSTANCES_GLSL
