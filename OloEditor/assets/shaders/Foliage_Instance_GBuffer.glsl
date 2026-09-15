// =============================================================================
// Foliage_Instance_GBuffer.glsl — Deferred G-Buffer variant of
// Foliage_Instance.glsl. Same instanced VS (wind + fade + rotation); FS writes
// material data into the 4-RT G-Buffer so foliage participates in the deferred
// lighting composite. Alpha-tested cutouts are expressed as hard `discard`
// (G-Buffer has no alpha blending).
//
// `emissive.a = 0.0` → lit (full PBR + directional shadow via DeferredLightingPass).
// Velocity captures camera + per-object motion and also reprojects per-fragment
// wind sway by re-evaluating the wind function at `u_PrevTime` in the VS and
// passing a prev-frame world position through to the fragment stage.
// =============================================================================

#type vertex
#version 460 core

// No OLO_INSTANCE_NO_FORWARD: the deferred fragment includes InstanceBlock.glsl
// for u_EntityID (picking), so the instance index has to be forwarded.
#include "include/FoliageInstanceVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;
layout(location = 6) in vec3 v_PrevWorldPos;
layout(location = 7) in float v_MeshCoverage;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
    vec4 _foliageImpostorParams0; // consumed by the impostor card only
    vec4 _foliageImpostorParams1; // consumed by the impostor card only
    // x = this draw is the authored mesh (1) or the flat card (0);
    // yz = the layer's mesh-to-card hand-over band (issue #1233).
    vec4 u_MeshParams;
    // xyz = the view position the hand-over is measured from, in the same
    // render-relative space as the instance pivots. NOT u_CameraPosition: the
    // shadow pass's camera is the light. See ShaderBindingLayout::FoliageUBO.
    vec4 u_MeshViewPos;
};

#include "include/FoliageInstanceGeometry.glsl"

// Mirror the vertex-stage ModelMatrices block so the entity-ID picking
// slot is available in the fragment stage. SPIR-V link validation rejects
// mismatched layouts so the padding fields stay identical.
#include "include/InstanceBlock.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
#endif

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;
// Baked lightmap irradiance target (G-Buffer RT5, issue #865). This shader
// draws no lightmapped receiver, but an MRT output it never writes is
// UNDEFINED in that attachment, and RT5's .a is a coverage flag — undefined
// there reads as "this pixel has baked GI" and the deferred ambient ladder
// shades it from whatever the target happened to hold. Writing vec4(0) is the
// explicit "no baked GI here" every non-lightmapped G-Buffer writer owes the
// lighting pass.
layout(location = 5) out vec4 o_GBufferBakedGI;

vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

void main()
{
    // Mesh-to-card hand-over (issue #1233). STOCHASTIC on purpose: the G-Buffer
    // has no alpha to blend, so the two draws partition the pixels between them
    // instead of each fading — see FoliageInstanceGeometry.glsl.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy))
        discard;

    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    vec3 albedo = texColor.rgb * v_Color;

    if (texColor.a < v_AlphaCutoff)
        discard;

    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.001)
        discard;

    // G-Buffer has no alpha blending — collapse fade into a hard discard
    // threshold instead of modulating alpha.
    float alpha = texColor.a * fadeFactor * v_Fade;
    if (alpha < 0.3)
        discard;

    vec3 N = normalize(v_Normal);
    // Foliage is primarily diffuse — non-metallic, rough, full AO.
    float metallic = 0.0;
    float roughness = 0.9;
    float ao = 1.0;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0); // lit

    // Camera + wind-reprojection velocity. v_PrevWorldPos already includes the
    // prev-frame wind displacement (evaluated at u_PrevTime in the VS).
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_GBufferVelocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferEntityID = u_EntityID;
    o_GBufferBakedGI = vec4(0.0); // no baked lightmap on this surface (issue #865)
}
