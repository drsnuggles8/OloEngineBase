#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform UniformBufferObject {
    mat4 u_ViewProjection;
    mat4 u_Model;
};

void main()
{
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(std140, binding = 1) uniform LightProperties {
    vec3 u_ObjectColor;
    vec3 u_LightColor;
};

void main()
{
    FragColor = vec4(u_LightColor * u_ObjectColor, 1.0);
}