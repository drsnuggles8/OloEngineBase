// =============================================================================
// VirtualMeshGBuffer.glsl — hardware raster path of the virtualized-geometry
// cluster pipeline (Nanite-style cluster LOD DAG, issue #629).
//
// Draws the clusters selected by VirtualClusterCull.comp through one
// glMultiDrawElementsIndirectCount call per virtual-mesh instance. There are
// no vertex attributes: geometry is pulled from the cluster vertex SSBO via
// gl_VertexIndex (the pooled cluster-local index buffer + per-command
// BaseVertex land it on the right pooled slot), and per-draw data comes from
// the VirtualDrawInfo UBO (one update per MDI call) + gl_DrawID indexing the
// instance's command segment.
//
// Fragment stage mirrors PBR_GBuffer.glsl exactly (same material UBO, same
// texture slots, same MRT encodings) so virtual geometry inherits deferred
// PBR + shadows + GTAO + SSR unchanged.
// =============================================================================

#type vertex
#version 460 core

// gl_BaseInstance keys this draw's VisibleCluster record (the cull wrote it as
// each command's BaseInstance) — used only by the debug visualization.
#extension GL_ARB_shader_draw_parameters : require

// Mirrors OloEngine::VirtualGpuVertex (VirtualMeshGpuData.h, 32 B std430)
struct VirtualGpuVertex {
    vec4 PositionU; // xyz mesh-local position, w = TexCoord.x
    vec4 NormalV;   // xyz mesh-local normal,   w = TexCoord.y
};

// Mirrors OloEngine::VirtualInstanceGpuRecord (224 B std430)
struct VirtualInstance {
    mat4 Transform;      // render-origin-relative
    mat4 PrevTransform;
    mat4 NormalMatrix;
    uint ClusterBase;
    uint ClusterCount;
    uint GroupBase;
    int  EntityID;
    float MaxScale;
    float ErrorThresholdPixels;
    uint CommandBase;
    uint Flags;
};

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Per-MDI-call draw info, uploaded by VirtualGeometryPass before each
// instance's glMultiDrawElementsIndirectCount (binding 49 = UBO_VIRTUAL_DRAW).
layout(std140, binding = 49) uniform VirtualDrawInfo {
    uint u_VirtualInstanceIndex; // instance every draw in this MDI call belongs to
    uint u_VirtualCommandBase;   // instance's segment base in the shared command buffer
    uint _vdPad0;
    uint _vdPad1;
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;
layout(location = 5) flat out int v_EntityID;
layout(location = 6) flat out uint v_DbgSlot; // gl_BaseInstance -> VisibleCluster record (debug only)

invariant gl_Position;

void main()
{
    VirtualInstance inst = instances[u_VirtualInstanceIndex];
    VirtualGpuVertex vert = vertices[gl_VertexIndex];
    v_DbgSlot = uint(gl_BaseInstanceARB);

    vec3 localPosition = vert.PositionU.xyz;
    vec3 localNormal = vert.NormalV.xyz;

    v_WorldPos = vec3(inst.Transform * vec4(localPosition, 1.0));
    v_Normal = mat3(inst.NormalMatrix) * localNormal;
    v_TexCoord = vec2(vert.PositionU.w, vert.NormalV.w);
    v_EntityID = inst.EntityID;

    v_ClipPosCurr = u_ViewProjection * vec4(v_WorldPos, 1.0);
    vec4 prevWorldPos = inst.PrevTransform * vec4(localPosition, 1.0);
    v_ClipPosPrev = u_PrevViewProjection * prevWorldPos;

    gl_Position = v_ClipPosCurr;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/VirtualDebugViz.glsl"

// Cluster + visible records for the debug visualization (already bound at these
// SSBO points by the cull; read only when u_DebugMode != 0). Layouts mirror
// OloEngine::VirtualClusterGpuRecord (64 B) / VirtualVisibleCluster (16 B).
struct VirtualClusterDbg {
    vec4 CullSphere; vec4 Cone;
    uint VertexBase; uint IndexBase; uint IndexCount; uint GroupIndex; uint RefinedGroup;
    uint Lod; uint _p1; uint _p2;
};
struct VisibleClusterDbg { uint InstanceIndex; uint ClusterIndex; uint _p0; uint _p1; };
layout(std430, binding = 33) readonly buffer VirtualClustersDbgBuf { VirtualClusterDbg dbgClusters[]; };
layout(std430, binding = 38) readonly buffer VirtualVisibleDbgBuf { VisibleClusterDbg dbgVisible[]; };

// PBR Material UBO (binding 2) — identical layout to PBR_GBuffer so the same
// PODMaterialData upload path works unchanged for virtual geometry.
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;
    vec4 u_EmissiveFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    int u_UseAlbedoMap;
    int u_UseNormalMap;
    int u_UseMetallicRoughnessMap;
    int u_UseAOMap;
    int u_UseEmissiveMap;
    int u_EnableIBL;
    int u_ApplyGammaCorrection;
    float u_AlphaCutoff;
    int u_EnableLightProbes;
    float u_IBLIntensity;
    int u_AlphaMode;        // 0=Opaque, 1=Mask, 2=Blend
    int _pbrPad2;
};

layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;
layout(location = 5) flat in int v_EntityID;
layout(location = 6) flat in uint v_DbgSlot;

layout(location = 0) out vec4 o_GBufferAlbedo;    // RGBA8       albedo + metallic
layout(location = 1) out vec4 o_GBufferNormal;    // RGBA16F     octNormal + roughness + ao
layout(location = 2) out vec4 o_GBufferEmissive;  // RGBA16F     emissive + flags
layout(location = 3) out vec2 o_GBufferVelocity;  // RG16F       screen-space velocity
layout(location = 4) out int  o_GBufferEntityID;  // RED_INTEGER picking entity ID

// Octahedral encode: unit normal -> [-1,1]^2 (same as PBR_GBuffer.glsl).
vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

void main()
{
    // glTF MASK alpha handling, identical to PBR_GBuffer.glsl
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     u_MetallicFactor, u_RoughnessFactor,
                                                     bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // sanitizeSurfaceNormal, not normalize: see PBR_GBuffer.glsl — a zero/NaN vertex normal
    // must not reach the octahedral G-Buffer encode.
    vec3 N = sanitizeSurfaceNormal(v_Normal, dFdx(v_WorldPos), dFdy(v_WorldPos));
    if (u_UseNormalMap == 1)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }

    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(emissive, 0.0);
    o_GBufferVelocity = velocity;
    o_GBufferEntityID = v_EntityID;

    // Debug visualization (no-op unless a debug mode is active). Resolve this
    // fragment's cluster + LOD from the draw's VisibleCluster record.
    if (u_DebugMode != 0)
    {
        uint ci = dbgVisible[v_DbgSlot].ClusterIndex;
        WriteVirtualDebug(ci, dbgClusters[ci].Lod);
    }
}
