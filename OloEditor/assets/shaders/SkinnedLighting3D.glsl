#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_BoneIndices;  // Bone indices (up to 4 per vertex)
layout(location = 4) in vec4 a_BoneWeights;   // Bone weights (must sum to 1.0)

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
};

// Bone matrices UBO (up to 100 bones)
layout(std140, binding = 5) uniform BoneMatrices {
    mat4 u_BoneMatrices[100];
};

// Model matrix uniform block
layout(std140, binding = 6) uniform ModelMatrix {
    mat4 u_Model;
};

// Outputs to fragment shader
layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec3 v_FragPos;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    // GPU Skinning: Calculate final vertex position and normal based on bone influences
    mat4 boneTransform = mat4(0.0);
    
    // Blend bone matrices based on weights
    for (int i = 0; i < 4; ++i)
    {
        int boneIndex = a_BoneIndices[i];
        float weight = a_BoneWeights[i];
        
        // Ensure bone index is within valid range
        if (boneIndex >= 0 && boneIndex < 100 && weight > 0.0)
        {
            boneTransform += weight * u_BoneMatrices[boneIndex];
        }
    }
    
    // If no bone influences (or all weights are 0), use identity matrix
    if (boneTransform == mat4(0.0))
    {
        boneTransform = mat4(1.0);
    }
    
    // Transform vertex position and normal by the blended bone matrix
    vec4 skinnedPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 skinnedNormal = mat3(boneTransform) * a_Normal;    // Transform to world space using model matrix
    vec4 worldPosition = u_Model * skinnedPosition;
    v_FragPos = worldPosition.xyz;
    v_Normal = normalize(mat3(u_Model) * skinnedNormal);
    v_TexCoord = a_TexCoord;
    
    // Final vertex position in clip space
    gl_Position = u_ViewProjection * worldPosition;
}

#type fragment
#version 450 core

// Input from vertex shader
layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec3 v_FragPos;
layout(location = 2) in vec2 v_TexCoord;

// Output
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
vec3 CalculatePointLight();
vec3 CalculateSpotLight();

void main()
{
    vec3 result;
    int lightType = int(u_ViewPosAndLightType.w);

    if (lightType == DIRECTIONAL_LIGHT)
        result = CalculateDirectionalLight();
    else if (lightType == POINT_LIGHT)
        result = CalculatePointLight();
    else if (lightType == SPOT_LIGHT)
        result = CalculateSpotLight();
    else
        result = vec3(1.0, 0.0, 1.0); // Magenta for error

    FragColor = vec4(result, 1.0);
}

vec3 CalculateDirectionalLight()
{    vec3 lightDir = normalize(-u_LightDirection.xyz);
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

vec3 CalculatePointLight()
{
    vec3 lightDir = normalize(u_LightPosition.xyz - v_FragPos);
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
        specular *= specularTexColor;    }
    
    // Attenuation
    float distance = length(u_LightPosition.xyz - v_FragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + u_LightAttParams.z * (distance * distance));
    
    // Ambient
    vec3 ambientResult = u_LightAmbient.xyz * ambient;
    
    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuseResult = u_LightDiffuse.xyz * diff * diffuse;
    
    // Specular
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specularResult = u_LightSpecular.xyz * spec * specular;
    
    // Apply attenuation
    ambientResult *= attenuation;
    diffuseResult *= attenuation;
    specularResult *= attenuation;
    
    return ambientResult + diffuseResult + specularResult;
}

vec3 CalculateSpotLight()
{
    vec3 lightDir = normalize(u_LightPosition.xyz - v_FragPos);
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
        specular *= specularTexColor;    }
    
    // Attenuation
    float distance = length(u_LightPosition.xyz - v_FragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + u_LightAttParams.z * (distance * distance));
    
    // Spotlight intensity
    float theta = dot(lightDir, normalize(-u_LightDirection.xyz));
    float epsilon = u_LightSpotParams.x - u_LightSpotParams.y;
    float intensity = clamp((theta - u_LightSpotParams.y) / epsilon, 0.0, 1.0);
    
    // Ambient
    vec3 ambientResult = u_LightAmbient.xyz * ambient;
    
    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuseResult = u_LightDiffuse.xyz * diff * diffuse;
    
    // Specular
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specularResult = u_LightSpecular.xyz * spec * specular;
    
    // Apply attenuation and spotlight intensity
    ambientResult *= attenuation;
    diffuseResult *= attenuation * intensity;
    specularResult *= attenuation * intensity;
    
    return ambientResult + diffuseResult + specularResult;
}
