// Shadow Depth Point Light Shader
// Used for point light shadow map generation (cubemap).
// Writes linear depth for correct cubemap shadow comparison.

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

layout(location = 0) out vec3 v_WorldPosition;

void main()
{
	vec4 worldPos = u_Model * vec4(a_Position, 1.0);
	v_WorldPosition = worldPos.xyz;
	gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPosition;

// Light position and far plane passed via Camera UBO (u_CameraPosition = light position)
layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition; // Repurposed as light position during point shadow pass
	float _padding0;       // Repurposed as far plane
};

void main()
{
	float lightDistance = length(v_WorldPosition - u_CameraPosition);
	float farPlane = _padding0;
	gl_FragDepth = lightDistance / farPlane;
}
