// =============================================================================
// Skybox_GBuffer.glsl — Deferred G-Buffer variant of Skybox.glsl.
//
// Writes the skybox colour into the G-Buffer's emissive attachment with the
// **unlit flag** set (RT2.a = 1.0). `DeferredLightingShared.glsl`'s
// `ComputeDeferredLit` short-circuits the PBR path when `emissiveFlags.a > 0.5`
// and returns the raw emissive colour, so skybox pixels composite identically
// to the existing forward `ForwardOverlayRenderPass` behaviour — but without
// needing a separate forward pass after lighting.
//
// Selected by `Renderer3D::DrawSkybox` when the deferred path is active.
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

layout(location = 0) out vec3 v_TexCoords;

void main()
{
    v_TexCoords = a_Position;

    // Remove translation from view matrix to center skybox around camera
    mat4 rotView = mat4(mat3(u_View));
    vec4 pos = u_Projection * rotView * vec4(a_Position, 1.0);

    // Snap to far plane so skybox sits behind all deferred geometry.
    gl_Position = pos.xyww;
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_TexCoords;

// G-Buffer MRT layout (matches GBuffer.h + PBR_GBuffer.glsl).
layout(location = 0) out vec4 o_GBufferAlbedo;    // RGBA8
layout(location = 1) out vec4 o_GBufferNormal;    // RGBA16F
layout(location = 2) out vec4 o_GBufferEmissive;  // RGBA16F — carries unlit flag in .a
layout(location = 3) out vec2 o_GBufferVelocity;  // RG16F

layout(binding = 9) uniform samplerCube u_Skybox;

void main()
{
    vec3 skyColor = texture(u_Skybox, v_TexCoords).rgb;

    // Albedo / normal / AO are unused for unlit surfaces; write zeros for
    // deterministic debug-channel output.
    o_GBufferAlbedo   = vec4(0.0);
    o_GBufferNormal   = vec4(0.0);
    // Pack the skybox sample into the emissive channel and set the unlit
    // flag (.a = 1.0) so `ComputeDeferredLit` returns the colour unshaded.
    o_GBufferEmissive = vec4(skyColor, 1.0);
    // Skybox is rigidly attached to the camera — no screen-space velocity.
    o_GBufferVelocity = vec2(0.0);
}
