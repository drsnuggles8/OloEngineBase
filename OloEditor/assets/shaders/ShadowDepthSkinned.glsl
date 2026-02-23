// Shadow Depth Shader for Skinned/Animated Meshes
// Used for directional and spot light shadow map generation.
// Applies bone transforms before rendering from the light's perspective.

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

layout(std140, binding = 4) uniform AnimationMatrices
{
	mat4 u_BoneMatrices[100];
};

void main()
{
	// Calculate bone transformation
	mat4 boneTransform = u_BoneMatrices[a_BoneIndices[0]] * a_BoneWeights[0];
	boneTransform += u_BoneMatrices[a_BoneIndices[1]] * a_BoneWeights[1];
	boneTransform += u_BoneMatrices[a_BoneIndices[2]] * a_BoneWeights[2];
	boneTransform += u_BoneMatrices[a_BoneIndices[3]] * a_BoneWeights[3];

	vec4 animatedPosition = boneTransform * vec4(a_Position, 1.0);
	gl_Position = u_ViewProjection * u_Model * animatedPosition;
}

#type fragment
#version 460 core

void main()
{
	// Depth is written automatically by the rasterizer
}
