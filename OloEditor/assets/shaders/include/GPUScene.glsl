#ifndef OLO_GPU_SCENE_GLSL
#define OLO_GPU_SCENE_GLSL

// std430 mirror of Renderer/GPUScene/GPUSceneTypes.h. Device addresses are
// represented as uvec2 so the OpenGL contract stays valid without requiring
// 64-bit integer extensions; Vulkan consumers can reconstruct the uint64_t.
//
// Member names and order are pinned by GPUSceneLayoutTest.cpp against the C++
// offsetof table: renaming, reordering or resizing a member on one side only
// fails that test.
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
    uint MaterialGeneration;
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

// GPUSceneMaterialFlag
#define OLO_GPU_SCENE_MATERIAL_ACTIVE (1u << 0)
#define OLO_GPU_SCENE_MATERIAL_PBR (1u << 1)
#define OLO_GPU_SCENE_MATERIAL_TWO_SIDED (1u << 2)
#define OLO_GPU_SCENE_MATERIAL_BLEND (1u << 3)
#define OLO_GPU_SCENE_MATERIAL_DEPTH_TEST (1u << 4)
#define OLO_GPU_SCENE_MATERIAL_DISABLE_SHADOW_CASTING (1u << 5)
#define OLO_GPU_SCENE_MATERIAL_IBL (1u << 6)
#define OLO_GPU_SCENE_MATERIAL_USE_TEXTURE_MAPS (1u << 7)
#define OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP (1u << 8)
#define OLO_GPU_SCENE_MATERIAL_METALLIC_ROUGHNESS_MAP (1u << 9)
#define OLO_GPU_SCENE_MATERIAL_NORMAL_MAP (1u << 10)
#define OLO_GPU_SCENE_MATERIAL_OCCLUSION_MAP (1u << 11)
#define OLO_GPU_SCENE_MATERIAL_EMISSIVE_MAP (1u << 12)
#define OLO_GPU_SCENE_MATERIAL_SPECULAR_MAP (1u << 13)

// A heap offset a consumer must not index with: the texture was not
// resolvable through the descriptor heap at extraction (GL without the
// bindless heap). Bind through the slot path instead.
#define OLO_GPU_SCENE_HEAP_OFFSET_UNRESOLVED 0xFFFFFFFFu

struct GPUSceneMaterial
{
    vec4 BaseColorFactor;
    vec4 EmissiveFactor;
    vec4 LegacyAmbient;
    vec4 LegacySpecular;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float AlphaCutoff;
    uint AlphaMode;
    uint ClosureVersion;
    uint Flags;
    uint AlbedoTextureIndex;
    uint AlbedoTextureGeneration;
    uint MetallicRoughnessTextureIndex;
    uint MetallicRoughnessTextureGeneration;
    uint NormalTextureIndex;
    uint NormalTextureGeneration;
    uint OcclusionTextureIndex;
    uint OcclusionTextureGeneration;
    uint EmissiveTextureIndex;
    uint EmissiveTextureGeneration;
    uint SpecularTextureIndex;
    uint SpecularTextureGeneration;
    uint AlbedoHeapOffset;
    uint MetallicRoughnessHeapOffset;
    uint NormalHeapOffset;
    uint OcclusionHeapOffset;
    uint EmissiveHeapOffset;
    uint SpecularHeapOffset;
    uint StableIndex;
    uint Generation;
};

// GPUSceneLightType — the same numbering as PBRCommon.glsl's *_LIGHT tags.
#define OLO_GPU_SCENE_LIGHT_DIRECTIONAL 0u
#define OLO_GPU_SCENE_LIGHT_POINT 1u
#define OLO_GPU_SCENE_LIGHT_SPOT 2u
#define OLO_GPU_SCENE_LIGHT_SPHERE_AREA 3u

// GPUSceneLightFlag
#define OLO_GPU_SCENE_LIGHT_ACTIVE (1u << 0)
#define OLO_GPU_SCENE_LIGHT_CAST_SHADOWS (1u << 1)

// Positions are render-relative, like GPUSceneInstance transforms.
struct GPUSceneLight
{
    vec4 PositionAndRange;   // xyz position (0 for directional), w range
    vec4 DirectionAndRadius; // xyz direction the light travels, w sphere radius
    vec4 ColorAndIntensity;  // rgb colour, w intensity
    vec4 ShapeParams;        // x cos(inner), y cos(outer), z quadratic attenuation, w spot falloff
    uint Type;
    uint Flags;
    uint StableIndex;
    uint Generation;
};

// GPUSceneEnvironmentFlag
#define OLO_GPU_SCENE_ENVIRONMENT_ACTIVE (1u << 0)
#define OLO_GPU_SCENE_ENVIRONMENT_IBL (1u << 1)
#define OLO_GPU_SCENE_ENVIRONMENT_MAP (1u << 2)

struct GPUSceneEnvironment
{
    uint EnvironmentIndex;
    uint EnvironmentGeneration;
    uint IrradianceIndex;
    uint IrradianceGeneration;
    uint PrefilterIndex;
    uint PrefilterGeneration;
    uint BRDFLutIndex;
    uint BRDFLutGeneration;
    uint EnvironmentHeapOffset;
    uint IrradianceHeapOffset;
    uint PrefilterHeapOffset;
    uint BRDFLutHeapOffset;
    float Intensity;
    uint Flags;
    uint StableIndex;
    uint Generation;
};

// Pass-local aliases. The portable GL SSBO namespace is full, so every GPU
// Scene buffer borrows a slot from a family the raster migration (#994)
// retires or rebinds per pass: 15/16/17 are the GPU instance cull's trio
// (SSBO_INSTANCE_DATA / _CULL_INPUT / _DRAW_INDIRECT), 9/10 are the Forward+
// per-type light buffers (SSBO_FPLUS_POINT_LIGHTS / _SPOT_LIGHTS). A consumer
// binds these immediately before use and may not treat them as global sticky
// state. A shader that includes this file may not declare, itself or through
// another include, a storage block at any of these five numbers
// (GPUSceneLayoutTest scans every shader's include closure for that).
layout(std430, binding = 15) readonly buffer OloGPUSceneInstances
{
    GPUSceneInstance g_GPUSceneInstances[];
};

layout(std430, binding = 16) readonly buffer OloGPUSceneGeometries
{
    GPUSceneGeometry g_GPUSceneGeometries[];
};

layout(std430, binding = 17) readonly buffer OloGPUSceneMaterials
{
    GPUSceneMaterial g_GPUSceneMaterials[];
};

layout(std430, binding = 9) readonly buffer OloGPUSceneLights
{
    GPUSceneLight g_GPUSceneLights[];
};

layout(std430, binding = 10) readonly buffer OloGPUSceneEnvironments
{
    GPUSceneEnvironment g_GPUSceneEnvironments[];
};

#endif // OLO_GPU_SCENE_GLSL
