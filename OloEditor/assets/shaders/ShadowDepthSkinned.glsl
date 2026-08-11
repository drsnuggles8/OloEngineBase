// Shadow Depth Shader for Skinned/Animated Meshes
// Used for directional and spot light shadow map generation.
// Applies bone transforms before rendering from the light's perspective.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5, decision A3): the SKINNED two-stream vertex pull.
// The engine `Vertex` stream (V1: 32 B -- vec3 position @0, vec3 normal @12,
// vec2 uv @24) arrives on the engine-wide vertex-pull binding 57. The BONE
// stream (V2: 32 B -- uvec4 BoneIDs @0, vec4 Weights @16) is a SECOND vertex
// buffer on the same VAO (MeshSource::BuildBoneInfluenceBuffer) and arrives on
// the reserved second pull binding 63, which AssembleAndPushRootData maps to
// VAO stream 1. Both strides are 8 floats. Bone IDs are u32 in memory and the
// pull view is float[], so they come back through floatBitsToInt -- exactly the
// reinterpretation GL performs for the `Int4` bone-id attribute. Pulled locals
// below main() carry the ATTRIBUTE NAMES, so the body is identical on both
// routes; the GL attribute branch is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloBonePull
{
    float v[];
} b_Bones;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;     // unused, but present in vertex layout
layout(location = 2) in vec2 a_TexCoord;   // unused, but present in vertex layout
layout(location = 3) in ivec4 a_BoneIndices;
layout(location = 4) in vec4 a_BoneWeights;
#endif

layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _padding0;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(std140, binding = 4) uniform AnimationMatrices
{
	mat4 u_BoneMatrices[100];
};

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
	vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
	vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
	int boneBase = gl_VertexIndex * 8;
	ivec4 a_BoneIndices = ivec4(floatBitsToInt(b_Bones.v[boneBase + 0]), floatBitsToInt(b_Bones.v[boneBase + 1]),
	                      floatBitsToInt(b_Bones.v[boneBase + 2]), floatBitsToInt(b_Bones.v[boneBase + 3]));
	vec4 a_BoneWeights = vec4(b_Bones.v[boneBase + 4], b_Bones.v[boneBase + 5], b_Bones.v[boneBase + 6], b_Bones.v[boneBase + 7]);
#endif
	OLO_INSTANCE_FORWARD();
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
