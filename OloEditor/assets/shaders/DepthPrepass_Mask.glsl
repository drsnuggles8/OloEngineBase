// =============================================================================
// DepthPrepass_Mask.glsl - Depth-only prepass shader for alpha-MASK static meshes
//
// MASK (glTF alpha-cutout) variant of DepthPrepass.glsl: the fragment stage
// re-runs the exact alpha test from PBR_MultiLight.glsl so cutout geometry
// (foliage cards, grates, chains) writes the same depth coverage the color
// pass expects — a plain opaque prepass would write depth for the discarded
// texels and clip holes into everything behind them.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// vertex data is READ, not fetched -- binding 57 is the engine-wide vertex-pull
// binding and the root struct carries this buffer's device address. The stream
// is the engine `Vertex` (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24),
// so the per-vertex stride is 8 floats. Pulled locals below main() carry the
// ATTRIBUTE NAMES, which keeps the body identical on both routes; the GL
// attribute branch is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 2) in vec2 a_TexCoord;
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
#include "include/InstanceBlock_Vertex.glsl"

layout(location = 2) out vec2 v_TexCoord;

invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    OLO_INSTANCE_FORWARD();
    v_TexCoord = a_TexCoord;
    vec3 worldPos = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 2) in vec2 v_TexCoord;

// Converted whole (§5c) — the alpha-test albedo is this shader's only sampler.
// It MUST carve the same depth coverage as the colour pass, so it samples the
// same material texture through the same per-material offset lane.
#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define OLO_MATERIAL_HEAP_READER 1
#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE
#endif

// Overdraw counter — see DepthPrepass.glsl. Written only for fragments that
// survive the MASK alpha test below, so the count matches shaded coverage.
layout(location = 0) out vec4 o_OverdrawCount;

// PBR Material UBO (binding 2) — full block layout must match PBR_MultiLight.glsl
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;     // Base color (albedo) with alpha
    vec4 u_EmissiveFactor;      // Emissive color
    float u_MetallicFactor;     // Metallic factor
    float u_RoughnessFactor;    // Roughness factor
    float u_NormalScale;        // Normal map scale
    float u_OcclusionStrength;  // AO strength
    int u_UseAlbedoMap;         // Use albedo texture
    int u_UseNormalMap;         // Use normal map
    int u_UseMetallicRoughnessMap; // Use metallic-roughness texture
    int u_UseAOMap;             // Use ambient occlusion map
    int u_UseEmissiveMap;       // Use emissive map
    int u_EnableIBL;            // Enable IBL
    int u_ApplyGammaCorrection; // Apply gamma correction in this pass
    float u_AlphaCutoff;        // Alpha cutoff for MASK mode
    int u_EnableLightProbes;    // Enable light probe indirect diffuse
    float u_IBLIntensity;       // Runtime IBL strength multiplier
    int u_AlphaMode;            // 0=Opaque, 1=Mask, 2=Blend
    int _pbrPad2;
    // Per-material heap offsets (issue #691 Phase 3). MUST mirror
    // PBRMaterialUBO::HeapOffsets — std140 shifts every later field if the two
    // layouts disagree, and this block is the LAST member so a missing
    // declaration reads garbage rather than failing to link.
    //   [0] albedo, metallicRoughness, normal, ao
    //   [1] emissive, environment, irradiance, prefilter
    //   [2] brdfLut, diffuse(legacy), specular(legacy), unused
    uvec4 u_MaterialHeapOffsets[3];
};

void main()
{
    // glTF MASK alpha test — identical to PBR_MultiLight.glsl so the prepass
    // depth coverage matches the color pass texel-for-texel.
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

    o_OverdrawCount = vec4(1.0);
}
