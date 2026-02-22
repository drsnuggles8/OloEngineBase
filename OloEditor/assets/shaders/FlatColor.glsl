// Flat Color Shader

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

layout(std140, binding = 3) uniform Model
{
	mat4 u_Transform;
};

void main()
{
	gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;

layout(std140, binding = 6) uniform FlatColorParams
{
	vec4 u_Color;
};

void main()
{
	o_Color = u_Color;
}
