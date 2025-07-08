#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
};

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec3 v_FragPos;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
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
    vec4 u_LightPosition;
    vec4 u_LightDirection;
    vec4 u_LightAmbient;
    vec4 u_LightDiffuse;
    vec4 u_LightSpecular;
    vec4 u_LightAttParams;    // (x = constant, y = linear, z = quadratic)
    vec4 u_LightSpotParams;   // (x = cutOff, y = outerCutOff)
    vec4 u_ViewPosAndLightType; // (xyz = viewPos, w = lightType)
};

layout(binding = 0) uniform sampler2D u_DiffuseMap;
layout(binding = 1) uniform sampler2D u_SpecularMap;

layout(std140, binding = 2) uniform MaterialProperties {
    vec4 u_MaterialAmbient;
    vec4 u_MaterialDiffuse;
    vec4 u_MaterialSpecular;
    vec4 u_MaterialEmissive;
    int u_UseTextureMaps;
    int _padding[3];
};

vec3 CalcDirectionalLight(vec3 normal, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    vec3 lightDir = normalize(-u_LightDirection.xyz);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    vec3 ambient = u_LightAmbient.xyz * diffuseColor;
    return ambient + diffuse + specular;
}

vec3 CalcPointLight(vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    vec3 lightDir = normalize(u_LightPosition.xyz - fragPos);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    float distance = length(u_LightPosition.xyz - fragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance +
        u_LightAttParams.z * (distance * distance));
    vec3 ambient = u_LightAmbient.xyz * diffuseColor;
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return ambient + diffuse + specular;
}

vec3 CalcSpotLight(vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    vec3 lightDir = normalize(u_LightPosition.xyz - fragPos);
    float theta = dot(lightDir, normalize(-u_LightDirection.xyz));
    float epsilon = u_LightSpotParams.x - u_LightSpotParams.y;
    float intensity = clamp((theta - u_LightSpotParams.y) / epsilon, 0.0, 1.0);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightDiffuse.xyz * (diff * diffuseColor);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_MaterialSpecular.w);
    vec3 specular = u_LightSpecular.xyz * (spec * specularColor);
    float distance = length(u_LightPosition.xyz - fragPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance +
        u_LightAttParams.z * (distance * distance));
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
    vec3 normal = normalize(v_Normal);
    vec3 viewDir = normalize(u_ViewPosAndLightType.xyz - v_FragPos);

    vec3 diffuseColor, specularColor;
    if (u_UseTextureMaps == 1) {
        diffuseColor = vec3(texture(u_DiffuseMap, v_TexCoord));
        specularColor = vec3(texture(u_SpecularMap, v_TexCoord));
    }
    else {
        diffuseColor = u_MaterialDiffuse.xyz;
        specularColor = u_MaterialSpecular.xyz;
    }

    vec3 result = vec3(0.0);
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
