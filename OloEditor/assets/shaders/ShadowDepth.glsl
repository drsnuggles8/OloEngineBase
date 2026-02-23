// Shadow Depth Shader
// Used for directional and spot light shadow map generation.
// Renders geometry from the light's perspective; depth is written automatically.

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _padding0;
};

layout(std140, binding = 3) uniform ModelMatrices
{
	mat4 u_Model;
	mat4 u_Normal;
	int u_EntityID;
	int _paddingEntity0;
	int _paddingEntity1;
	int _paddingEntity2;
};

void main()
{
	gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

void main()
{
	// Depth is written automatically by the rasterizer
}
