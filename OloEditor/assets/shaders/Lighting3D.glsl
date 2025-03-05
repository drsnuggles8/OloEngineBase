#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(std140, binding = 0) uniform UniformBufferObject {
    mat4 u_ViewProjection;
    mat4 u_Model;
};

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec3 v_FragPos;

void main()
{
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(transpose(inverse(u_Model))) * a_Normal; // This is expensive, could be optimized

    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec3 v_FragPos;

layout(location = 0) out vec4 FragColor;

layout(std140, binding = 1) uniform LightProperties {
    vec4 u_ObjectColor;     // Using vec4 for proper std140 alignment
    vec4 u_LightColor;      // Using vec4 for proper std140 alignment
    vec4 u_LightPos;        // Using vec4 for proper std140 alignment
    vec4 u_ViewPos;         // Using vec4 for proper std140 alignment
    vec4 u_LightingParams;  // x: ambient strength, y: specular strength, z: shininess, w: padding
};

void main()
{
    // Normalize normal vector
    vec3 normal = normalize(v_Normal);

    // Get lighting parameters
    float ambientStrength = u_LightingParams.x;
    float specularStrength = u_LightingParams.y;
    float shininess = u_LightingParams.z;

    // Ambient component
    vec3 ambient = ambientStrength * u_LightColor.rgb;

    // Diffuse component
    vec3 lightDir = normalize(u_LightPos.xyz - v_FragPos);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * u_LightColor.rgb;

    // Specular component
    vec3 viewDir = normalize(u_ViewPos.xyz - v_FragPos);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * u_LightColor.rgb;

    // Combine all components
    vec3 result = (ambient + diffuse + specular) * u_ObjectColor.rgb;
    FragColor = vec4(result, 1.0);
}