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

const int DIRECTIONAL_LIGHT = 0;
const int POINT_LIGHT = 1;
const int SPOT_LIGHT = 2;

layout(std140, binding = 1) uniform LightProperties {
    vec4 u_MaterialAmbient;
    vec4 u_MaterialDiffuse;
    vec4 u_MaterialSpecular; // (x,y,z = specular, w = shininess)
    vec4 u_Padding1;

    vec4 u_LightPosition;
    vec4 u_LightDirection;
    vec4 u_LightAmbient;
    vec4 u_LightDiffuse;
    vec4 u_LightSpecular;
    vec4 u_LightAttParams;    // (x = constant, y = linear, z = quadratic)
    vec4 u_LightSpotParams;   // (x = cutOff, y = outerCutOff)

    vec4 u_ViewPosAndLightType; // (x,y,z = viewPos, w = lightType)
};

layout(std140, binding = 2) uniform TextureFlags {
    int u_UseTextureMaps;
};

layout(binding = 3) uniform sampler2D u_DiffuseMap;
layout(binding = 4) uniform sampler2D u_SpecularMap;

vec3 CalculateDirectionalLight();

void main()
{   
    // Apply lighting calculations
    vec3 result;
    int lightType = int(u_ViewPosAndLightType.w);

    if (lightType == DIRECTIONAL_LIGHT)
        result = CalculateDirectionalLight();
    else if (lightType == POINT_LIGHT)
        result = CalculateDirectionalLight(); // Use directional for simplicity
    else if (lightType == SPOT_LIGHT)
        result = CalculateDirectionalLight(); // Use directional for simplicity
    else
        result = vec3(1.0, 0.0, 1.0); // Magenta for error

    FragColor = vec4(result, 1.0);
}

vec3 CalculateDirectionalLight()
{
    vec3 lightDir = normalize(-u_LightDirection.xyz);
    vec3 norm = normalize(v_Normal);
    vec3 viewDir = normalize(u_ViewPosAndLightType.xyz - v_FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    
    // Get material properties
    vec3 ambient = u_MaterialAmbient.xyz;
    vec3 diffuse = u_MaterialDiffuse.xyz;
    vec3 specular = u_MaterialSpecular.xyz;
    float shininess = u_MaterialSpecular.w;
    
    // Apply textures if enabled
    if (u_UseTextureMaps == 1)
    {
        vec3 diffuseTexColor = texture(u_DiffuseMap, v_TexCoord).rgb;
        vec3 specularTexColor = texture(u_SpecularMap, v_TexCoord).rgb;
        diffuse *= diffuseTexColor;
        specular *= specularTexColor;
    }
    
    // Ambient
    vec3 ambientResult = u_LightAmbient.xyz * ambient;
    
    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuseResult = u_LightDiffuse.xyz * diff * diffuse;
    
    // Specular
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specularResult = u_LightSpecular.xyz * spec * specular;
    
    return ambientResult + diffuseResult + specularResult;
}
