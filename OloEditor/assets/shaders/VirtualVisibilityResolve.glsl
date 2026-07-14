// =============================================================================
// VirtualVisibilityResolve.glsl — visibility-buffer material resolve for the
// virtualized-geometry pipeline (issue #629, slice 4).
//
// Fullscreen pass, one draw per virtual-mesh instance (the engine's material
// model binds textures per draw — per-pixel material selection would need
// bindless). For every pixel the compute software rasterizer won: decode
// (swRecord, triangle), refetch the triangle, rebuild perspective-correct
// barycentrics at the pixel, interpolate attributes, evaluate the instance's
// PBR material, and write the exact same G-Buffer MRT contract as
// PBR_GBuffer.glsl / VirtualMeshGBuffer.glsl. gl_FragDepth replays the
// visibility buffer's stored depth bits, so hardware-raster clusters and
// software-raster clusters compose through the normal depth test.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_Position.xy * 0.5 + 0.5;
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 460 core

// PBRCommon owns the ONE normal-map decode + TBN construction shared with the
// hardware raster (VirtualMeshGBuffer.glsl) and the classic path. This pass used
// to hand-roll its own and they drifted — sampled-vs-reconstructed tangent-space
// z (issue #440 silently re-introduced for every software-rasterized cluster) and
// an inverted bitangent. See the block above getNormalFromMap in PBRCommon.glsl.
#include "include/PBRCommon.glsl"
#include "include/VirtualDebugViz.glsl"

// Mirrors OloEngine::VirtualClusterGpuRecord (64 B std430)
struct VirtualCluster {
    vec4 CullSphere;
    vec4 Cone;
    uint VertexBase;
    uint IndexBase;
    uint IndexCount;
    uint GroupIndex;
    uint RefinedGroup;
    uint Lod; uint _p1; uint _p2;
};

// Mirrors OloEngine::VirtualInstanceGpuRecord (224 B std430)
struct VirtualInstance {
    mat4 Transform;
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

// Mirrors OloEngine::VirtualGpuVertex (32 B std430)
struct VirtualGpuVertex {
    vec4 PositionU;
    vec4 NormalV;
};

// Mirrors OloEngine::VirtualVisibleCluster (16 B std430)
struct VisibleCluster {
    uint InstanceIndex;
    uint ClusterIndex;
    uint _p0; uint _p1;
};

layout(std430, binding = 33) readonly buffer VirtualClusters { VirtualCluster clusters[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };
layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 42) readonly buffer VirtualIndices { uint localIndices[]; };
layout(std430, binding = 40) readonly buffer VirtualSwList {
    uint Count;
    uint _h0; uint _h1; uint _h2;
    VisibleCluster Records[];
} swList;
layout(std430, binding = 41) readonly buffer VirtualVisbuffer { uvec2 pixels[]; }; // .x payload, .y depth bits

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

// Per-draw info (binding 49 = UBO_VIRTUAL_DRAW): which instance this resolve
// draw shades, plus the visibility-buffer dimensions.
layout(std140, binding = 49) uniform VirtualDrawInfo {
    uint u_VirtualInstanceIndex;
    uint u_VirtualCommandBase;
    uint u_VirtualViewportWidth;
    uint u_VirtualViewportHeight;
};

// PBR Material UBO (binding 2) — identical layout to PBR_GBuffer
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
    int u_AlphaMode;
    int _pbrPad2;
};

layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;

// Octahedral encode (same as PBR_GBuffer.glsl)
vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Screen-space barycentrics of `pixel` (window coords, pixel-centre samples)
// for the window-space triangle (s0, s1, s2). Unnormalized weights match the
// software rasterizer's edge functions exactly.
//
// The signed area is NEGATIVE for a back-facing (clockwise in window space)
// triangle, which the rasterizer now emits for TWO-SIDED materials
// (kFlagTwoSided, issue #629). Divide by the signed area, not by max(area, eps):
// clamping it to +1e-12 turned every back-face barycentric into ~1e12 garbage.
// Only a genuinely zero-area (degenerate) triangle needs the epsilon floor.
vec3 ScreenBarycentrics(vec2 pixel, vec2 s0, vec2 s1, vec2 s2)
{
    float w0 = (s1.x - pixel.x) * (s2.y - pixel.y) - (s2.x - pixel.x) * (s1.y - pixel.y);
    float w1 = (s2.x - pixel.x) * (s0.y - pixel.y) - (s0.x - pixel.x) * (s2.y - pixel.y);
    float w2 = (s0.x - pixel.x) * (s1.y - pixel.y) - (s1.x - pixel.x) * (s0.y - pixel.y);
    float area = w0 + w1 + w2;
    float safeArea = (abs(area) > 1e-12) ? area : 1e-12;
    return vec3(w0, w1, w2) / safeArea;
}

void main()
{
    uvec2 pixelCoord = uvec2(gl_FragCoord.xy);
    if (pixelCoord.x >= u_VirtualViewportWidth || pixelCoord.y >= u_VirtualViewportHeight)
        discard;

    uvec2 packedPixel = pixels[pixelCoord.y * u_VirtualViewportWidth + pixelCoord.x];
    if (packedPixel.y == 0xFFFFFFFFu)
        discard; // cleared — nothing software-rasterized here

    uint swRecordIndex = packedPixel.x >> 9u;
    uint triangleIndex = packedPixel.x & 0x1FFu;
    if (swRecordIndex >= swList.Count)
        discard;

    VisibleCluster record = swList.Records[swRecordIndex];
    if (record.InstanceIndex != u_VirtualInstanceIndex)
        discard; // another instance's pixel — resolved by its own draw

    VirtualInstance inst = instances[record.InstanceIndex];
    VirtualCluster cluster = clusters[record.ClusterIndex];
    if (triangleIndex * 3u + 2u >= cluster.IndexCount)
        discard;

    uint i0 = localIndices[cluster.IndexBase + triangleIndex * 3u + 0u];
    uint i1 = localIndices[cluster.IndexBase + triangleIndex * 3u + 1u];
    uint i2 = localIndices[cluster.IndexBase + triangleIndex * 3u + 2u];
    VirtualGpuVertex v0 = vertices[cluster.VertexBase + i0];
    VirtualGpuVertex v1 = vertices[cluster.VertexBase + i1];
    VirtualGpuVertex v2 = vertices[cluster.VertexBase + i2];

    mat4 mvp = u_ViewProjection * inst.Transform;
    vec4 c0 = mvp * vec4(v0.PositionU.xyz, 1.0);
    vec4 c1 = mvp * vec4(v1.PositionU.xyz, 1.0);
    vec4 c2 = mvp * vec4(v2.PositionU.xyz, 1.0);

    vec2 viewport = vec2(float(u_VirtualViewportWidth), float(u_VirtualViewportHeight));
    vec2 s0 = (c0.xy / c0.w * 0.5 + 0.5) * viewport;
    vec2 s1 = (c1.xy / c1.w * 0.5 + 0.5) * viewport;
    vec2 s2 = (c2.xy / c2.w * 0.5 + 0.5) * viewport;

    // Perspective-correct barycentrics at the pixel centre (+1px offsets for
    // analytic UV gradients feeding textureGrad).
    vec2 pixel = gl_FragCoord.xy;
    vec3 bScreen = ScreenBarycentrics(pixel, s0, s1, s2);
    vec3 invW = 1.0 / vec3(c0.w, c1.w, c2.w);
    vec3 bPersp = bScreen * invW;
    bPersp /= max(bPersp.x + bPersp.y + bPersp.z, 1e-12);

    vec2 uv0 = vec2(v0.PositionU.w, v0.NormalV.w);
    vec2 uv1 = vec2(v1.PositionU.w, v1.NormalV.w);
    vec2 uv2 = vec2(v2.PositionU.w, v2.NormalV.w);
    vec2 uv = uv0 * bPersp.x + uv1 * bPersp.y + uv2 * bPersp.z;

    vec3 bScreenX = ScreenBarycentrics(pixel + vec2(1.0, 0.0), s0, s1, s2) * invW;
    bScreenX /= max(bScreenX.x + bScreenX.y + bScreenX.z, 1e-12);
    vec3 bScreenY = ScreenBarycentrics(pixel + vec2(0.0, 1.0), s0, s1, s2) * invW;
    bScreenY /= max(bScreenY.x + bScreenY.y + bScreenY.z, 1e-12);
    vec2 uvDx = (uv0 * bScreenX.x + uv1 * bScreenX.y + uv2 * bScreenX.z) - uv;
    vec2 uvDy = (uv0 * bScreenY.x + uv1 * bScreenY.y + uv2 * bScreenY.z) - uv;

    // Interpolated world-space position + normal
    vec3 wp0 = vec3(inst.Transform * vec4(v0.PositionU.xyz, 1.0));
    vec3 wp1 = vec3(inst.Transform * vec4(v1.PositionU.xyz, 1.0));
    vec3 wp2 = vec3(inst.Transform * vec4(v2.PositionU.xyz, 1.0));
    vec3 worldPos = wp0 * bPersp.x + wp1 * bPersp.y + wp2 * bPersp.z;

    // Analytic screen-space derivatives of world position at the same +1px offsets
    // — exactly what dFdx/dFdy(v_WorldPos) yields on the hardware path, but valid
    // in a fullscreen pass where the neighbouring pixel may be a different
    // triangle/cluster/instance entirely. Feeds the shared TBN below.
    vec3 wpDx = (wp0 * bScreenX.x + wp1 * bScreenX.y + wp2 * bScreenX.z) - worldPos;
    vec3 wpDy = (wp0 * bScreenY.x + wp1 * bScreenY.y + wp2 * bScreenY.z) - worldPos;

    vec3 localNormal = v0.NormalV.xyz * bPersp.x + v1.NormalV.xyz * bPersp.y + v2.NormalV.xyz * bPersp.z;
    vec3 N = normalize(mat3(inst.NormalMatrix) * localNormal);

    // Material evaluation — same helpers/encodings as PBR_GBuffer.glsl. UV
    // derivatives are analytic (screen-space dFdx would mix neighbouring
    // triangles at cluster edges in a fullscreen resolve).
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= textureGrad(u_AlbedoMap, uv, uvDx, uvDy).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

    vec3 albedo = u_BaseColorFactor.rgb;
    if (u_UseAlbedoMap == 1)
        albedo *= textureGrad(u_AlbedoMap, uv, uvDx, uvDy).rgb;

    float metallic = u_MetallicFactor;
    float roughness = u_RoughnessFactor;
    if (u_UseMetallicRoughnessMap == 1)
    {
        vec2 mr = textureGrad(u_MetallicRoughnessMap, uv, uvDx, uvDy).bg;
        metallic *= mr.x;
        roughness *= mr.y;
    }

    float ao = 1.0;
    if (u_UseAOMap == 1)
        ao = mix(1.0, textureGrad(u_AOMap, uv, uvDx, uvDy).r, u_OcclusionStrength);

    vec3 emissive = u_EmissiveFactor.rgb;
    if (u_UseEmissiveMap == 1)
        emissive *= textureGrad(u_EmissiveMap, uv, uvDx, uvDy).rgb;

    if (u_UseNormalMap == 1)
    {
        // ONE shared implementation with the hardware raster + the classic path
        // (PBRCommon::getNormalFromMap): same z reconstruction (#440 — a BC5/RGTC
        // two-channel map has blue = 0, so sampling z would invert the normal),
        // same handedness, same UV-degenerate NaN guard. This pass supplies the
        // derivatives analytically because a fullscreen resolve cannot use dFdx.
        N = getNormalFromMapGrad(u_NormalMap, uv, wpDx, wpDy, uvDx, uvDy, N, u_NormalScale);
    }

    // Screen-space velocity: current NDC from the fragment position, previous
    // from the interpolated previous-frame clip position (same convention as
    // the hardware path's v_ClipPosPrev interpolation).
    mat4 prevMvp = u_PrevViewProjection * inst.PrevTransform;
    vec4 pc0 = prevMvp * vec4(v0.PositionU.xyz, 1.0);
    vec4 pc1 = prevMvp * vec4(v1.PositionU.xyz, 1.0);
    vec4 pc2 = prevMvp * vec4(v2.PositionU.xyz, 1.0);
    vec4 prevClip = pc0 * bPersp.x + pc1 * bPersp.y + pc2 * bPersp.z;
    vec2 ndcCurr = (gl_FragCoord.xy / viewport) * 2.0 - 1.0;
    vec2 ndcPrev = prevClip.xy / max(prevClip.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(emissive, 0.0);
    o_GBufferVelocity = velocity;
    o_GBufferEntityID = inst.EntityID;

    // Debug visualization (no-op unless a debug mode is active).
    WriteVirtualDebug(record.ClusterIndex, cluster.Lod);

    // Replay the visibility buffer's exact depth so SW and HW clusters compose
    // through the standard depth test.
    gl_FragDepth = uintBitsToFloat(packedPixel.y);
}
