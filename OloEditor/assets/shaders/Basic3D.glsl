#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;

layout(std140, binding = 0) uniform UniformBufferObject {
    mat4 u_ViewProjection;
    mat4 u_Model;
};

layout(location = 0) out vec3 v_Color;

void main()
{
    v_Color = a_Color;
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_Color;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = vec4(v_Color, 1.0);
}