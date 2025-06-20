#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_BoneIndices;
layout(location = 4) in vec4 a_BoneWeights;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
};

layout(std140, binding = 6) uniform ModelMatrix {
    mat4 u_Model;
};

layout(std140, binding = 5) uniform BoneMatrices {
    mat4 u_BoneMatrices[100];
};

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec3 v_FragPos;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    // Simple GPU Skinning
    mat4 boneTransform = mat4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        int boneIndex = a_BoneIndices[i];
        float weight = a_BoneWeights[i];
        if (boneIndex >= 0 && boneIndex < 100 && weight > 0.0)
        {
            boneTransform += weight * u_BoneMatrices[boneIndex];
        }
    }
    if (boneTransform == mat4(0.0))
    {
        boneTransform = mat4(1.0);
    }
    
    vec4 skinnedPos = boneTransform * vec4(a_Position, 1.0);
    vec3 skinnedNormal = mat3(boneTransform) * a_Normal;
    
    vec4 worldPos = u_Model * skinnedPos;
    v_FragPos = worldPos.xyz;
    v_Normal = normalize(mat3(u_Model) * skinnedNormal);
    v_TexCoord = a_TexCoord;
    
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec3 v_FragPos;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 0) out vec4 FragColor;

void main()
{
    // Simple test color
    FragColor = vec4(1.0, 0.0, 1.0, 1.0); // Magenta
}
