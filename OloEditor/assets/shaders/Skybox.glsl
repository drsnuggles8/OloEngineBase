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
    mat4 rotView = mat4(mat3(u_View)); // Extract only rotation part
    vec4 pos = u_Projection * rotView * vec4(a_Position, 1.0);

    // Set z to w to ensure skybox is always at far plane
    gl_Position = pos.xyww;
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_TexCoords;

// MRT outputs — all forward-rendered geometry must write all 3 targets
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int  o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

layout(binding = 9) uniform samplerCube u_Skybox;

void main()
{
    o_Color = texture(u_Skybox, v_TexCoords);
    o_EntityID = -1;          // No entity for skybox (sentinel for picking)
    o_ViewNormal = vec2(0.0); // Neutral normal for skybox pixels
}
