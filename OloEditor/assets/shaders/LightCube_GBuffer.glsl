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

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

layout(location = 0) out vec4 v_ClipPosCurr;
layout(location = 1) out vec4 v_ClipPosPrev;

void main()
{
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

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;

void main()
{
    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);

    o_GBufferAlbedo   = vec4(0.0);
    o_GBufferNormal   = vec4(0.0);
    // Bright white unlit — matches forward LightCube behaviour.
    o_GBufferEmissive = vec4(1.0, 1.0, 1.0, 1.0);
    o_GBufferVelocity = (ndcCurr - ndcPrev) * 0.5;
}
