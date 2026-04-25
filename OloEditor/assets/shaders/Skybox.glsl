#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Previous-frame VP drives the velocity output on scene FB RT3 so TAA
    // sees correct per-pixel motion on skybox pixels. A w=0 input vector
    // zeros the translation columns of VP, yielding rotation-only clip
    // space (matching the current frame's `mat4(mat3(u_View))` trick).
    mat4 u_PrevViewProjection;
};

layout(location = 0) out vec3 v_TexCoords;
layout(location = 1) out vec4 v_ClipPosCurr;
layout(location = 2) out vec4 v_ClipPosPrev;

void main()
{
    v_TexCoords = a_Position;

    // Remove translation from view matrix to center skybox around camera
    mat4 rotView = mat4(mat3(u_View)); // Extract only rotation part
    vec4 pos = u_Projection * rotView * vec4(a_Position, 1.0);

    // Set z to w to ensure skybox is always at far plane
    gl_Position = pos.xyww;

    // Velocity reconstruction: use the full prev VP with w=0 so translation
    // drops out. xy in NDC then matches a rotation-only projection (the
    // current-frame path uses `mat4(mat3(u_View))` to the same end).
    v_ClipPosCurr = vec4(pos.xy, pos.w, pos.w);
    v_ClipPosPrev = u_PrevViewProjection * vec4(a_Position, 0.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_TexCoords;
layout(location = 1) in vec4 v_ClipPosCurr;
layout(location = 2) in vec4 v_ClipPosPrev;

// MRT outputs — all forward-rendered geometry must write all 3 targets
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int  o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity: TAA reprojects skybox pixels using camera rotation
// only (translation is irrelevant at infinity).
layout(location = 3) out vec2 o_Velocity;

layout(binding = 9) uniform samplerCube u_Skybox;

void main()
{
    o_Color = texture(u_Skybox, v_TexCoords);
    o_EntityID = -1;          // No entity for skybox (sentinel for picking)
    o_ViewNormal = vec2(0.0); // Neutral normal for skybox pixels

    // NDC-space velocity (units: half-NDC) matching PBR_MultiLight so TAA
    // sees consistently-scaled motion vectors across all forward shaders.
    vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
    vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
