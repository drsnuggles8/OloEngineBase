// =============================================================================
// MaterialPreview.glsl
//
// Minimal forward-PBR shader for content-browser material thumbnails.
// Renders an icosphere (or arbitrary mesh) with a metallic-roughness
// material under a hardcoded three-point light rig (key + fill + rim).
// No IBL, no shadows, no instance UBO — driven by `AssetPreviewRenderer`
// outside the Renderer3D frame graph.
//
// Why a dedicated shader instead of reusing PBR.glsl: the main PBR path
// expects the Renderer3D pipeline's UBO bindings (camera, lights, IBL,
// shadow maps, instance SSBO, post-process state). Wiring all of that up
// just to render a 256-pixel sphere thumbnail is more code than the
// thumbnail itself.
//
// The engine's shader pipeline cross-compiles GLSL → SPIR-V → GLSL, and
// SPIR-V forbids non-opaque uniforms outside a uniform block — so all
// scalar / vector / matrix uniforms here live inside `PreviewBlock`.
// Texture samplers stay as opaque uniforms (allowed). Binding 50 sits
// well above the engine's documented UBO range (0..33 per
// `ShaderBindingLayout.h`) so we never collide with anything Renderer3D
// might leave bound when the preview is invoked between frames.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 50) uniform PreviewBlock
{
    mat4 u_Model;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _pad0;
    vec3 u_AlbedoFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_EmissiveFactor;
    int u_UseAlbedoMap;
    int u_UseMetalnessMap;
    int u_UseRoughnessMap;
    int _pad1;
    int _pad2;
    int _pad3;
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = normalize(mat3(u_Model) * a_Normal);
    v_TexCoord = a_TexCoord;

    gl_Position = u_Projection * u_View * vec4(v_WorldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_Color;

layout(std140, binding = 50) uniform PreviewBlock
{
    mat4 u_Model;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _pad0;
    vec3 u_AlbedoFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_EmissiveFactor;
    int u_UseAlbedoMap;
    int u_UseMetalnessMap;
    int u_UseRoughnessMap;
    int _pad1;
    int _pad2;
    int _pad3;
};

layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetalnessMap;
layout(binding = 2) uniform sampler2D u_RoughnessMap;

// Three-point rig for the preview turntable. Key is warm, fill is
// cool and softer, rim hits from behind to give the silhouette a
// little pop. Tuned for an icosphere viewed straight on; works
// reasonably for arbitrary meshes too.
const vec3  c_KeyDir            = normalize(vec3(-0.4, -0.7, -0.5));
const vec3  c_KeyColor          = vec3(3.6, 3.4, 3.1);
const vec3  c_FillDir           = normalize(vec3(0.7, 0.2, 0.6));
const vec3  c_FillColor         = vec3(0.55, 0.6, 0.8);
const vec3  c_RimDir            = normalize(vec3(0.0, 0.3, 1.0));
const vec3  c_RimColor          = vec3(1.4, 1.3, 1.1);
const vec3  c_AmbientColor      = vec3(0.045, 0.05, 0.06);
const float c_PI                = 3.14159265359;

// ---- GGX / Smith / Schlick helpers (Karis 2013 / UE4 PBR) ------------------

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (c_PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- Direct lighting evaluation --------------------------------------------

vec3 EvaluateDirectionalLight(vec3 N, vec3 V, vec3 lightDir, vec3 lightColor,
                              vec3 albedo, float metallic, float roughness, vec3 F0)
{
    vec3 L = normalize(-lightDir);
    vec3 H = normalize(V + L);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    return (kD * albedo / c_PI + specular) * lightColor * NdotL;
}

void main()
{
    vec3 albedo = u_AlbedoFactor;
    if (u_UseAlbedoMap != 0)
    {
        vec4 sampled = texture(u_AlbedoMap, v_TexCoord);
        albedo = sampled.rgb * u_AlbedoFactor;
    }

    float metallic = u_MetallicFactor;
    if (u_UseMetalnessMap != 0)
        metallic = clamp(texture(u_MetalnessMap, v_TexCoord).r * u_MetallicFactor, 0.0, 1.0);

    float roughness = u_RoughnessFactor;
    if (u_UseRoughnessMap != 0)
        roughness = clamp(texture(u_RoughnessMap, v_TexCoord).r * u_RoughnessFactor, 0.04, 1.0);
    roughness = max(roughness, 0.04);

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    Lo += EvaluateDirectionalLight(N, V, c_KeyDir, c_KeyColor, albedo, metallic, roughness, F0);
    Lo += EvaluateDirectionalLight(N, V, c_FillDir, c_FillColor, albedo, metallic, roughness, F0);
    Lo += EvaluateDirectionalLight(N, V, c_RimDir, c_RimColor, albedo, metallic, roughness, F0);

    // Cheap horizon-glow ambient: brightens silhouettes slightly,
    // approximates IBL contribution without paying a cubemap lookup.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    vec3 ambient = c_AmbientColor * albedo + 0.06 * rim * c_RimColor;

    vec3 emissive = albedo * u_EmissiveFactor;

    vec3 color = ambient + Lo + emissive;

    color = color / (color + vec3(1.0));                     // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));                     // gamma correction

    o_Color = vec4(color, 1.0);
}
