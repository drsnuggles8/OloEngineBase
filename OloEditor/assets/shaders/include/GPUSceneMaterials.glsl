#ifndef OLO_GPU_SCENE_MATERIALS_GLSL
#define OLO_GPU_SCENE_MATERIALS_GLSL

// The canonical material table. Aliases SSBO_INSTANCE_DRAW_INDIRECT, which no
// raster draw declares, so an ordinary mesh shader can take this one while it
// still reads per-draw InstanceData at 15.
// See include/GPUScene.glsl for the record contract and the per-kind slot map.
#include "GPUScene.glsl"

layout(std430, binding = 17) readonly buffer OloGPUSceneMaterials
{
    GPUSceneMaterial g_GPUSceneMaterials[];
};

#endif // OLO_GPU_SCENE_MATERIALS_GLSL
