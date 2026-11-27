// =============================================================================
// MaterialPreview.glsl
//
// Minimal forward-PBR shader for content-browser material thumbnails.
// Renders an icosphere (or arbitrary mesh) with a metallic-roughness
// material under a hardcoded three-point light rig (key + fill + rim).
// No IBL, no shadows, no instance UBO — driven by `AssetPreviewRenderer`
// outside the Renderer3D frame graph.
//
// Why a dedicated shader instead of reusing the main PBR_MultiLight path:
// that path expects the Renderer3D pipeline's UBO bindings (camera, lights, IBL,
// shadow maps, instance SSBO, post-process state). Wiring all of that up
// just to render a 256-pixel sphere thumbnail is more code than the
// thumbnail itself.
//
// The engine's shader pipeline cross-compiles GLSL → SPIR-V → GLSL, and
// SPIR-V forbids non-opaque uniforms outside a uniform block — so all
// scalar / vector / matrix uniforms here live inside `PreviewBlock`.
// Texture samplers stay as opaque uniforms (allowed). All bindings
// match `ShaderBindingLayout.h`: UBO 34 (UBO_PREVIEW), albedo at
// TEX_DIFFUSE (0), roughness at TEX_ROUGHNESS (6), metallic at
// TEX_METALLIC (7). `ShaderReflectionBinding` enforces this round-trip.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 34) uniform PreviewBlock
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

layout(std140, binding = 34) uniform PreviewBlock
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

// Sampler slots match the engine's documented PBR layout
// (ShaderBindingLayout.h TEX_DIFFUSE / TEX_ROUGHNESS / TEX_METALLIC) so
// `ShaderReflectionBinding` recognises them. The metallic sampler is
// named `u_MetallicMap` (not `u_MetalnessMap`) to match TEX_METALLIC's
// validator pattern — the engine uses "Metallic" everywhere; the API
// data side still calls the parameter "Metalness" (per glTF 2.0's
// metallic-roughness convention).
#include "include/BindlessHeap.glsl"
#include "include/PBRCommon.glsl"
#ifdef OLO_BINDLESS
#define u_AlbedoMap OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#define u_RoughnessMap OLO_HEAP_TEX_2D(6)  // TEX_ROUGHNESS
#define u_MetallicMap OLO_HEAP_TEX_2D(7)  // TEX_METALLIC
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 6) uniform sampler2D u_RoughnessMap;
layout(binding = 7) uniform sampler2D u_MetallicMap;
#endif

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

vec3 EvaluateDirectionalLight(vec3 N, vec3 V, vec3 lightDir, vec3 lightColor,
                              vec3 albedo, float metallic, float roughness)
{
    vec3 L = -lightDir;
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);
    return closureV2Evaluate(N, V, L, albedo, metallic, roughness) * lightColor * NdotL;
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
        metallic = clamp(texture(u_MetallicMap, v_TexCoord).r * u_MetallicFactor, 0.0, 1.0);

    float roughness = u_RoughnessFactor;
    if (u_UseRoughnessMap != 0)
        roughness = clamp(texture(u_RoughnessMap, v_TexCoord).r * u_RoughnessFactor, 0.04, 1.0);
    roughness = max(roughness, 0.04);

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    vec3 Lo = vec3(0.0);
    Lo += EvaluateDirectionalLight(N, V, c_KeyDir, c_KeyColor, albedo, metallic, roughness);
    Lo += EvaluateDirectionalLight(N, V, c_FillDir, c_FillColor, albedo, metallic, roughness);
    Lo += EvaluateDirectionalLight(N, V, c_RimDir, c_RimColor, albedo, metallic, roughness);

    // Cheap horizon-glow ambient: brightens silhouettes slightly,
    // approximates IBL contribution without paying a cubemap lookup.
    // (1-NdotV)^3 as a multiply chain — no pow() exp2/log2 per thumbnail pixel.
    float rimBase = 1.0 - max(dot(N, V), 0.0);
    float rim = rimBase * rimBase * rimBase;
    vec3 ambient = c_AmbientColor * albedo + 0.06 * rim * c_RimColor;

    vec3 emissive = albedo * u_EmissiveFactor;

    vec3 color = ambient + Lo + emissive;

    color = color / (color + vec3(1.0));                     // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));                     // gamma correction

    o_Color = vec4(color, 1.0);
}
