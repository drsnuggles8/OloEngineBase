#type vertex
#version 450 core

// Input vertex attributes
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Transformation matrices
layout(std140, binding = 0) uniform UniformBufferObject {
    mat4 u_ViewProjection;
    mat4 u_Model;
};

// Output to fragment shader
layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec3 v_FragPos;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    // Calculate fragment position in world space
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    
    // Transform normal to world space
    v_Normal = mat3(transpose(inverse(u_Model))) * a_Normal;
    
    // Pass texture coordinates to fragment shader
    v_TexCoord = a_TexCoord;

    // Calculate final clip space position
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

// Input from vertex shader
layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec3 v_FragPos;
layout(location = 2) in vec2 v_TexCoord;

// Output color
layout(location = 0) out vec4 FragColor;

// Light type constants
const int DIRECTIONAL_LIGHT = 0;
const int POINT_LIGHT = 1;
const int SPOT_LIGHT = 2;

// Material properties
struct Material {
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    float shininess;
};

// Light properties
struct Light {
    vec3 position;
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    
    // Attenuation parameters (for point and spotlight)
    float constant;
    float linear;
    float quadratic;
    
    // Spotlight parameters
    float cutOff;
    float outerCutOff;
};

// Material and light properties using std140 layout for exact memory alignment
layout(std140, binding = 1) uniform LightProperties {
    // Material properties (4 vec4s)
    vec4 u_MaterialAmbient;       // offset 0
    vec4 u_MaterialDiffuse;       // offset 16
    vec4 u_MaterialSpecular;      // offset 32 (x,y,z = specular, w = shininess)
    vec4 u_Padding1;              // offset 48 (padding for alignment)
    
    // Light properties (6 vec4s)
    vec4 u_LightPosition;         // offset 64
    vec4 u_LightDirection;        // offset 80
    vec4 u_LightAmbient;          // offset 96
    vec4 u_LightDiffuse;          // offset 112
    vec4 u_LightSpecular;         // offset 128
    vec4 u_LightAttParams;        // offset 144 (x = constant, y = linear, z = quadratic)
    vec4 u_LightSpotParams;       // offset 160 (x = cutOff, y = outerCutOff)
    
    // View position and light type (1 vec4)
    vec4 u_ViewPosAndLightType;   // offset 176 (xyz = viewPos, w = lightType)
};

// Texture samplers
layout(binding = 0) uniform sampler2D u_DiffuseMap;
layout(binding = 1) uniform sampler2D u_SpecularMap;

// Flag to determine if we should use texture maps
layout(binding = 2) uniform UseTextureBlock {
    int u_UseTextureMaps;
};

// Calculate light values for a directional light
vec3 CalcDirectionalLight(vec3 normal, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    vec3 lightDir = normalize(-u_LightDirection.xyz);
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    
    // Specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    
    // Combine
    vec3 ambient = u_LightAmbient.xyz * diffuseColor;
    return ambient + diffuse + specular;
}

// Calculate light values for a point light with attenuation
vec3 CalcPointLight(vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    // Light direction
    vec3 lightDir = normalize(u_LightPosition.xyz - fragPos);
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    
    // Specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    
    // Attenuation
    float distance = length(u_LightPosition.xyz - fragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + 
                             u_LightAttParams.z * (distance * distance));
    
    // Combine with attenuation
    vec3 ambient = u_LightAmbient.xyz * diffuseColor;
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    
    return ambient + diffuse + specular;
}

// Calculate light values for a spotlight with soft edges
vec3 CalcSpotLight(vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    // Light direction and cutoff comparison
    vec3 lightDir = normalize(u_LightPosition.xyz - fragPos);
    float theta = dot(lightDir, normalize(-u_LightDirection.xyz));
    float epsilon = u_LightSpotParams.x - u_LightSpotParams.y; // inner - outer
    float intensity = clamp((theta - u_LightSpotParams.y) / epsilon, 0.0, 1.0);
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    
    // Specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    
    // Attenuation
    float distance = length(u_LightPosition.xyz - fragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + 
                             u_LightAttParams.z * (distance * distance));
    
    // Combine with attenuation and spotlight intensity
    vec3 ambient = u_LightAmbient.xyz * diffuseColor;
    diffuse *= intensity;
    specular *= intensity;
    
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    
    return ambient + diffuse + specular;
}

void main()
{
    // Normalize normal vector
    vec3 normal = normalize(v_Normal);
    vec3 viewDir = normalize(u_ViewPosAndLightType.xyz - v_FragPos);
    
    // Get diffuse and specular colors (either from textures or material)
    vec3 diffuseColor, specularColor;
    if (u_UseTextureMaps == 1) {
        diffuseColor = vec3(texture(u_DiffuseMap, v_TexCoord));
        specularColor = vec3(texture(u_SpecularMap, v_TexCoord));
    } else {
        diffuseColor = u_MaterialDiffuse.xyz;
        specularColor = u_MaterialSpecular.xyz;
    }
    
    vec3 result = vec3(0.0);
    
    // Calculate lighting based on light type
    int lightType = int(u_ViewPosAndLightType.w);
    if (lightType == DIRECTIONAL_LIGHT) {
        result = CalcDirectionalLight(normal, viewDir, diffuseColor, specularColor);
    }
    else if (lightType == POINT_LIGHT) {
        result = CalcPointLight(normal, v_FragPos, viewDir, diffuseColor, specularColor);
    }
    else if (lightType == SPOT_LIGHT) {
        result = CalcSpotLight(normal, v_FragPos, viewDir, diffuseColor, specularColor);
    }
    
    FragColor = vec4(result, 1.0);
}