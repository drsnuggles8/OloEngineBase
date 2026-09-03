#ifndef OLO_GPU_SCENE_GEOMETRIES_GLSL
#define OLO_GPU_SCENE_GEOMETRIES_GLSL

// The canonical geometry table. Aliases SSBO_INSTANCE_CULL_INPUT.
// See include/GPUScene.glsl for the record contract and the per-kind slot map.
#include "GPUScene.glsl"

layout(std430, binding = 16) readonly buffer OloGPUSceneGeometries
{
    GPUSceneGeometry g_GPUSceneGeometries[];
};

#endif // OLO_GPU_SCENE_GEOMETRIES_GLSL
