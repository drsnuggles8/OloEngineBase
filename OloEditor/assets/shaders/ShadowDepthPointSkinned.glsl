// Shadow Depth Point Light Shader for Skinned/Animated Meshes
// Used for point light shadow map generation (cubemap).
// Applies bone transforms before rendering from the light's perspective.
// Writes linear depth for correct cubemap shadow comparison.

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;     // unused, but present in vertex layout
layout(location = 2) in vec2 a_TexCoord;   // unused, but present in vertex layout
layout(location = 3) in ivec4 a_BoneIndices;
layout(location = 4) in vec4 a_BoneWeights;

layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition; // Repurposed as light position during point shadow pass
	float _padding0;       // Repurposed as far plane
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

layout(std140, binding = 4) uniform AnimationMatrices
{
	mat4 u_BoneMatrices[100];
};

layout(location = 0) out vec3 v_WorldPosition;

void main()
{
	// Calculate bone transformation
	mat4 boneTransform = u_BoneMatrices[a_BoneIndices[0]] * a_BoneWeights[0];
	boneTransform += u_BoneMatrices[a_BoneIndices[1]] * a_BoneWeights[1];
	boneTransform += u_BoneMatrices[a_BoneIndices[2]] * a_BoneWeights[2];
	boneTransform += u_BoneMatrices[a_BoneIndices[3]] * a_BoneWeights[3];

	vec4 animatedPosition = boneTransform * vec4(a_Position, 1.0);
	vec4 worldPos = u_Model * animatedPosition;
	v_WorldPosition = worldPos.xyz;
	gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPosition;

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
