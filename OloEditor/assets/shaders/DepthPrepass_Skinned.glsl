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

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;     // unused, but present in the skinned vertex layout
layout(location = 2) in vec2 a_TexCoord;   // unused
layout(location = 3) in ivec4 a_BoneIDs;
layout(location = 4) in vec4 a_BoneWeights;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Instance transforms SSBO (binding 15)
#include "include/InstanceBlock_Vertex.glsl"

// Bone Matrices UBO (binding 4)
layout(std140, binding = 4) uniform BoneMatrices {
    mat4 u_BoneTransforms[100];
};

invariant gl_Position;

void main()
{
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
