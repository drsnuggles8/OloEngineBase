#ifndef OLO_GPU_SCENE_GLSL
#define OLO_GPU_SCENE_GLSL

// std430 mirror of Renderer/GPUScene/GPUSceneTypes.h. Device addresses are
// represented as uvec2 so the OpenGL contract stays valid without requiring
// 64-bit integer extensions; Vulkan consumers can reconstruct the uint64_t.
struct GPUSceneTransform
{
    vec4 Row0;
    vec4 Row1;
    vec4 Row2;
};

struct GPUSceneInstance
{
    GPUSceneTransform CurrentTransform;
    GPUSceneTransform PreviousTransform;
    uint GeometryIndex;
    uint GeometryGeneration;
    uint MaterialIndex;
    uint StableIndex;
    uint VisibilityMask;
    uint Flags;
    uint Generation;
    uint Pad0;
};

struct GPUSceneGeometry
{
    uint VertexBufferIndex;
    uint VertexBufferGeneration;
    uint IndexBufferIndex;
    uint IndexBufferGeneration;
    uvec2 VertexAddress;
    uvec2 IndexAddress;
    uint VertexFormat;
    uint IndexFormat;
    uint FirstIndex;
    uint IndexCount;
    int BaseVertex;
    uint VertexCount;
    uint Generation;
    uint Flags;
};

// Pass-local aliases of SSBO_INSTANCE_DATA / SSBO_INSTANCE_CULL_INPUT. The
// portable GL namespace is full, so a consumer must bind these immediately
// before use and may not treat them as global sticky state.
layout(std430, binding = 15) readonly buffer OloGPUSceneInstances
{
    GPUSceneInstance g_GPUSceneInstances[];
};

layout(std430, binding = 16) readonly buffer OloGPUSceneGeometries
{
    GPUSceneGeometry g_GPUSceneGeometries[];
};

#endif // OLO_GPU_SCENE_GLSL
