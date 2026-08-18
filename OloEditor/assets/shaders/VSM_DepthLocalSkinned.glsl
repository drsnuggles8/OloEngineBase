// Virtual Shadow Map depth raster — skinned mesh casters into LOCAL light layers
// (issue #703).
//
// The local twin of VSM_DepthSkinned.glsl, and it inherits that file's design
// whole: skinned casters do not go through the GPU cull, because their bone
// palette is bound per caster and cannot share an instanced batch. The CPU tests
// each caster's bounds against every active layer's light SPHERE (not its
// frustum — a perspective corner behind the near plane divides by a negative w,
// see VSM_CullLocalCasters.comp), writes one record per surviving layer, and
// issues one instanced draw.
//
// The vertex stage is VSM_DepthSkinned's skinning followed by VSM_DepthLocal's
// sub-rect scale. Neither half is new; only the combination is.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5, decision A3): the SKINNED two-stream vertex pull —
// engine `Vertex` on binding 57, the bone stream on the reserved binding 63.
// Both strides are 8 floats; bone IDs are u32 read back through floatBitsToInt,
// exactly the reinterpretation GL performs for the `Int4` attribute.
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
layout(location = 1) in vec3 a_Normal;   // unused, but present in the vertex layout
layout(location = 2) in vec2 a_TexCoord; // unused, but present in the vertex layout
layout(location = 3) in ivec4 a_BoneIndices;
layout(location = 4) in vec4 a_BoneWeights;
#endif

#include "include/VirtualShadowResources.glsl"
#include "include/VirtualShadowDrawList.glsl"

layout(std140, binding = 4) uniform AnimationMatrices
{
	mat4 u_BoneMatrices[100];
};

layout(location = 0) flat out uint v_VSMLocalLayer;
layout(location = 1) flat out uint v_VSMLocalMip;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
	int boneBase = gl_VertexIndex * 8;
	ivec4 a_BoneIndices = ivec4(floatBitsToInt(b_Bones.v[boneBase + 0]), floatBitsToInt(b_Bones.v[boneBase + 1]),
	                            floatBitsToInt(b_Bones.v[boneBase + 2]), floatBitsToInt(b_Bones.v[boneBase + 3]));
	vec4 a_BoneWeights = vec4(b_Bones.v[boneBase + 4], b_Bones.v[boneBase + 5], b_Bones.v[boneBase + 6], b_Bones.v[boneBase + 7]);
#endif
	mat4 boneTransform = u_BoneMatrices[a_BoneIndices[0]] * a_BoneWeights[0];
	boneTransform += u_BoneMatrices[a_BoneIndices[1]] * a_BoneWeights[1];
	boneTransform += u_BoneMatrices[a_BoneIndices[2]] * a_BoneWeights[2];
	boneTransform += u_BoneMatrices[a_BoneIndices[3]] * a_BoneWeights[3];

	VSMDrawInstance instance = b_DrawInstances[u_VSMPassParams.x + uint(gl_InstanceIndex)];
	uint layer = min(instance.LocalLayer, uint(VSM_MAX_LOCAL_LAYERS - 1));
	uint rasterMip = min(b_LocalRasterMip[layer], uint(VSM_LOCAL_MIP_COUNT - 1));

	v_VSMLocalLayer = layer;
	v_VSMLocalMip = rasterMip;

	vec4 animatedPosition = boneTransform * vec4(a_Position, 1.0);
	vec4 clipPos = b_LocalLights[layer].ViewProjectionRaster * instance.Transform * animatedPosition;

	// Sub-rect scale — see VSM_DepthLocal.glsl.
	float s = 1.0 / float(1u << rasterMip);
	clipPos.xy = (clipPos.xy + clipPos.w) * s - clipPos.w;
	gl_Position = clipPos;
}

#type fragment
#version 460 core

#include "include/VirtualShadowLocalRasterStage.glsl"
