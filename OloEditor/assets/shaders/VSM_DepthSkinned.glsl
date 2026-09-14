// Virtual Shadow Map depth raster — skinned mesh casters (issue #702).
//
// Skinned casters do NOT go through VSM_CullCasters. Their bone palette is bound
// per caster, so they cannot share an instanced batch with anything else, and a
// GPU cull would have to be paid per caster anyway. Instead the CPU frustum-tests
// each caster against the clip levels, writes one instance record per surviving
// level, and issues a single instanced draw — the clip level still travels in the
// record, so the shape of the vertex stage matches VSM_Depth.glsl exactly.
//
// Losing the HPB test costs nothing in correctness: the fragment stage's
// not-dirty early-out still refuses to touch a cached page, so a skinned caster
// submitted over clean pages burns vertex work and writes nothing. And a skinned
// caster is animating by definition, which means its pages were invalidated and
// are dirty anyway.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, decision A3): the SKINNED two-stream vertex pull —
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

// Skeletal deformation producer (#1226): declares the bone palette at binding 4
// and owns the skinning math shared with every other skinned consumer.
#include "include/SkeletalDeformation.glsl"

layout(location = 0) flat out uint v_VSMClipLevel;

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
	// Deformed vertex from the shared producer (#1226). Before that, this pass
	// accumulated the bone palette inline with no bone-ID bounds test and no
	// zero-weight guard: an unweighted vertex accumulated a zero matrix and
	// collapsed onto the model origin, so its shadow detached from the caster
	// that the colour and depth passes drew at the rest position.
	vec4 animatedPosition = OloDeformSkinnedPosition(a_Position, a_BoneIndices, a_BoneWeights);

	VSMDrawInstance instance = b_DrawInstances[u_VSMPassParams.x + uint(gl_InstanceIndex)];
	v_VSMClipLevel = instance.ClipLevel;

	gl_Position = u_VSMClips[instance.ClipLevel].ViewProjectionRaster * instance.Transform * animatedPosition;
}

#type fragment
#version 460 core

#include "include/VirtualShadowRasterStage.glsl"
