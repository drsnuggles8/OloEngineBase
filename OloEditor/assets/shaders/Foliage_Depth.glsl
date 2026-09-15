// =============================================================================
// Foliage_Depth.glsl - Shadow depth pass for instanced foliage
//
// Matches Foliage_Instance.glsl's geometry stream and, crucially, its
// PLACEMENT: every decision about where a plant's vertices go and which of the
// layer's two shapes owns a pixel comes from include/FoliageInstanceGeometry.glsl,
// the same functions the beauty and G-Buffer stages call (issue #1233, fourth
// criterion). A shadow cast by a quad while the lit plant is an authored pine
// passes every CPU test and reads downstream as a completely different bug.
//
// It keeps a vertex stage of its own rather than including
// FoliageInstanceVertexStage.glsl because it runs under a different contract:
// the shadow camera UBO carries the light VP and no previous-frame matrix, and
// no wind field is bound, so it uses the legacy sine wind both stages fall back
// to. Everything geometric is shared; only that contract differs.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V8 foliage two-stream pull — stream 0 = the
// 32-byte {vec3 position, vec3 normal, vec2 uv} geometry vertex on the
// engine-wide binding 57 (the card quad, or the layer's authored plant mesh),
// stream 1 = FoliageRenderer's 48-byte per-instance VB {PositionScale,
// RotationHeight, ColorAlpha} on the reserved stream-1 binding 63, indexed by
// gl_InstanceIndex. Pulled locals under the attribute names in main() keep the
// body shared (FoliageInstanceVertexStage.glsl carries the canonical comment).
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloBonePull
{
    float v[];
} b_Instances;
#define OLO_PULLED_VERTEX 1
#else
// Per-vertex attributes (card quad or authored mesh — same layout)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Per-instance attributes
layout(location = 3) in vec4 a_PositionScale;  // xyz = terrain-local pos, w = scale
layout(location = 4) in vec4 a_RotationHeight; // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 5) in vec4 a_ColorAlpha;     // rgb = tint, a = alpha cutoff
#endif

// Camera UBO (binding 0) — contains light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
// Foliage uploads ONE shared InstanceData entry for all N pulled instances
// (see FoliageRenderer::Render) — per-instance data rides the 48-byte
// instance stream instead. OLO_INSTANCE_SINGLE keeps the include from
// indexing that one entry by gl_InstanceIndex (GL: garbage read, Vulkan:
// device-losing page fault at high instance counts).
#define OLO_INSTANCE_SINGLE 1
// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

// Foliage UBO (binding 12)
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
    vec4 _foliageImpostorParams0; // consumed by the impostor card only
    vec4 _foliageImpostorParams1; // consumed by the impostor card only
    // x = this draw is the authored mesh (1) or the flat card (0);
    // yz = the layer's mesh-to-card hand-over band (issue #1233).
    vec4 u_MeshParams;
    // xyz = the view position the hand-over is measured from, in the same
    // render-relative space as the instance pivots. NOT u_CameraPosition: the
    // shadow pass's camera is the light. See ShaderBindingLayout::FoliageUBO.
    vec4 u_MeshViewPos;
};

#include "include/FoliageInstanceGeometry.glsl"

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out float v_AlphaCutoff;
layout(location = 2) out float v_MeshCoverage;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
    int instBase = gl_InstanceIndex * 12;
    vec4 a_PositionScale = vec4(b_Instances.v[instBase + 0], b_Instances.v[instBase + 1],
                                b_Instances.v[instBase + 2], b_Instances.v[instBase + 3]);
    vec4 a_RotationHeight = vec4(b_Instances.v[instBase + 4], b_Instances.v[instBase + 5],
                                 b_Instances.v[instBase + 6], b_Instances.v[instBase + 7]);
    vec4 a_ColorAlpha = vec4(b_Instances.v[instBase + 8], b_Instances.v[instBase + 9],
                             b_Instances.v[instBase + 10], b_Instances.v[instBase + 11]);
#endif
    OLO_INSTANCE_FORWARD();
    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float height = a_RotationHeight.y;
    bool isAuthoredMesh = u_MeshParams.x > 0.5;

    vec3 rotatedPos = foliageInstanceRotation(rotation) *
                      foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh);

    // Wind (must match the main shader for consistent shadows — same function,
    // so it cannot merely resemble it). The wind FIELD is not bound under the
    // shadow camera, so this is the legacy branch both stages share.
    rotatedPos += foliageLegacyWindOffset(a_PositionScale.xz, u_Time, u_WindSpeed, u_WindStrength,
                                          a_Position.y);

    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos = (u_Model * vec4(instancePos + rotatedPos, 1.0)).xyz;

    // Same per-instance hand-over as the beauty pass, measured from the render
    // origin rather than the camera for exactly the reason this stage exists:
    // u_CameraPosition is the LIGHT's here, and a hand-over keyed on it would
    // shadow the card where the lit frame drew the mesh.
    vec3 pivotRenderRel = (u_Model * vec4(instancePos, 1.0)).xyz;
    v_MeshCoverage = foliageMeshCoverage(distance(pivotRenderRel, u_MeshViewPos.xyz),
                                         u_MeshParams.y, u_MeshParams.z);

    v_TexCoord = a_TexCoord;
    v_AlphaCutoff = a_ColorAlpha.a;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);

    // Collapse whole instances this draw does not own — see the same guard in
    // FoliageInstanceVertexStage.glsl.
    if (isAuthoredMesh ? (v_MeshCoverage <= 0.0) : (v_MeshCoverage >= 1.0))
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    }
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in float v_AlphaCutoff;
layout(location = 2) in float v_MeshCoverage;

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
#endif

// Foliage UBO (binding 12) — shared with vertex stage
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
    vec4 _foliageImpostorParams0; // consumed by the impostor card only
    vec4 _foliageImpostorParams1; // consumed by the impostor card only
    vec4 u_MeshParams;
    vec4 u_MeshViewPos;
};

#include "include/FoliageInstanceGeometry.glsl"

void main()
{
    // The same partition rule, over the same per-instance coverage, so the
    // caster set tracks the drawn set as the hand-over crosses (issue #1233).
    // The dither is keyed on gl_FragCoord, which is the SHADOW MAP's here — so
    // inside the band an individual shadow-map texel may come from the other
    // side than the lit pixel it shadows. That is a sub-pixel difference at
    // matching coverage, which is what a dithered LOD is; what it cannot do is
    // shadow a quad where the lit frame drew a pine, because the coverage both
    // passes partition is computed per instance from the same pivot.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy))
        discard;

    float alpha = texture(u_DiffuseTexture, v_TexCoord).a;
    if (alpha < v_AlphaCutoff)
        discard;
}
