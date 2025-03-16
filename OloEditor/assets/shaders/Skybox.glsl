#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform Camera
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_ViewPos;
};

layout(location = 0) out vec3 v_TexCoords;

void main()
{
    v_TexCoords = a_Position;
    
    // Remove translation from the view matrix to keep the skybox centered around the camera
    mat4 viewNoTranslation = mat4(mat3(u_View));
    
    // The depth buffer trick - setting z = w after projection to ensure maximum depth value
    vec4 pos = u_Projection * viewNoTranslation * vec4(a_Position, 1.0);
    gl_Position = pos.xyww;
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec3 v_TexCoords;

layout(binding = 0) uniform samplerCube u_Skybox;

void main()
{
    o_Color = texture(u_Skybox, v_TexCoords);
}