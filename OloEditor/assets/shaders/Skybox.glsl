#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform UniformBufferObject {
    mat4 u_ViewProjection;
    mat4 u_Model;
};

layout(location = 0) out vec3 v_TexCoords;

void main()
{
    v_TexCoords = a_Position;
    
    // Remove translation from view-projection matrix to keep skybox centered on camera
    mat4 viewProjection = u_ViewProjection;
    viewProjection[3] = vec4(0.0, 0.0, 0.0, 1.0);
    
    vec4 pos = viewProjection * vec4(a_Position, 1.0);
    
    // Ensure skybox is always drawn at maximum depth
    gl_Position = pos.xyww;
}

#type fragment
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 v_TexCoords;

layout(binding = 0) uniform samplerCube u_SkyboxTexture;

void main()
{
    FragColor = texture(u_SkyboxTexture, v_TexCoords);
}