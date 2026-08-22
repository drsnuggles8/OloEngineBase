// =============================================================================
// DepthPrepass_Skinned.glsl - Depth-only prepass shader for skinned meshes
//
// Skinned counterpart of DepthPrepass.glsl. The bone accumulation and position
// math replicate the vertex stages of PBR_MultiLight_Skinned.glsl /
// PBR_GBuffer_Skinned.glsl exactly (minus the prev-frame velocity path, which
// does not feed gl_Position); `invariant gl_Position` keeps the prepass depth
// bit-identical to the GL_LEQUAL color pass.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, decision A3): the SKINNED two-stream vertex pull.
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
layout(location = 1) in vec3 a_Normal;     // unused, but present in the skinned vertex layout
layout(location = 2) in vec2 a_TexCoord;   // unused
layout(location = 3) in ivec4 a_BoneIDs;
layout(location = 4) in vec4 a_BoneWeights;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Instance transforms SSBO (binding 15)
// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

// Bone Matrices UBO (binding 4)
layout(std140, binding = 4) uniform BoneMatrices {
    mat4 u_BoneTransforms[100];
};

// ROUTE PARITY, not a bindless conversion (issue #691, glsl-shaders §7a-bis).
// This shader declares no samplers, so it has nothing to convert and would
// never mention OLO_BINDLESS on its own. It must still follow the COLOUR pass
// onto the raw-GLSL route, because `invariant gl_Position` below is a promise
// between TWO PROGRAMS and `invariant` cannot keep it across two different
// compiler front-ends. Left behind on the SPIR-V route while PBR_MultiLight
// moved, this pass wrote depth the colour pass then failed LEQUAL against, in
// blotches, on curved surfaces only.
#define OLO_BINDLESS_ROUTE_PARITY 1

invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
    int boneBase = gl_VertexIndex * 8;
    ivec4 a_BoneIDs = ivec4(floatBitsToInt(b_Bones.v[boneBase + 0]), floatBitsToInt(b_Bones.v[boneBase + 1]),
                          floatBitsToInt(b_Bones.v[boneBase + 2]), floatBitsToInt(b_Bones.v[boneBase + 3]));
    vec4 a_BoneWeights = vec4(b_Bones.v[boneBase + 4], b_Bones.v[boneBase + 5], b_Bones.v[boneBase + 6], b_Bones.v[boneBase + 7]);
#endif
    OLO_INSTANCE_FORWARD();
    // Bone accumulation mirrors PBR_MultiLight_Skinned / PBR_GBuffer_Skinned.
    mat4 boneTransform = mat4(0.0);
    float totalWeight = a_BoneWeights.x + a_BoneWeights.y + a_BoneWeights.z + a_BoneWeights.w;
    if (totalWeight > 0.001)
    {
        for (int i = 0; i < 4; ++i)
        {
            int boneID = a_BoneIDs[i];
            if (boneID >= 0 && boneID < 100)
            {
                boneTransform += u_BoneTransforms[boneID] * a_BoneWeights[i];
            }
        }
    }
    else
    {
        // Vertex has no bone influence — pass through without skinning
        boneTransform = mat4(1.0);
    }

    vec4 localPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 worldPos = vec3(u_Model * localPosition);
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

// Overdraw counter — see DepthPrepass.glsl. Discarded in the normal depth
// prepass (colour mask off); accumulated additively by the overdraw debug view.
layout(location = 0) out vec4 o_OverdrawCount;

void main()
{
    o_OverdrawCount = vec4(1.0);
}
