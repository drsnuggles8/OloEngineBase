// =============================================================================
// LightCube_GBuffer.glsl — Deferred G-Buffer variant of LightCube.glsl.
//
// Debug/gizmo cubes for light source visualisation. Written into the G-Buffer
// as **unlit** emissive white so they appear at full brightness through the
// deferred lighting pass instead of being shaded like a regular PBR surface.
// Selected by `Renderer3D::DrawLightCube` when the deferred path is active.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is Renderer3D::DrawLightCube (deferred variant), which
// draws s_Data.CubeMesh = MeshPrimitives::CreateCube() — the engine `Vertex`
// (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24), so the stride is
// 8 floats even though this stage only needs the position. The GL attribute
// branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

layout(location = 0) out vec4 v_ClipPosCurr;
layout(location = 1) out vec4 v_ClipPosPrev;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_ClipPosCurr = u_ViewProjection * worldPos;

    // Per-entity previous transform — gizmo cubes translate with their
    // owning light so motion blur should reflect that.
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    v_ClipPosPrev = u_PrevViewProjection * prevWorldPos;

    gl_Position = v_ClipPosCurr;
}

#type fragment
#version 460 core

layout(location = 0) in vec4 v_ClipPosCurr;
layout(location = 1) in vec4 v_ClipPosPrev;

// Mirror the vertex-stage UBO block so the entity ID picking slot is
// available in the fragment stage too. SPIR-V link validation rejects
// mismatched layouts, so the padding fields stay identical.
#include "include/InstanceBlock.glsl"

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;

void main()
{
    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);

    o_GBufferAlbedo   = vec4(0.0);
    o_GBufferNormal   = vec4(0.0);
    // Bright white unlit — matches forward LightCube behaviour.
    o_GBufferEmissive = vec4(1.0, 1.0, 1.0, 1.0);
    o_GBufferVelocity = (ndcCurr - ndcPrev) * 0.5;
    o_GBufferEntityID = u_EntityID;
}
