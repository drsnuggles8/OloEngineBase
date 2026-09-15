// =============================================================================
// Foliage_Impostor_GBuffer.glsl — the DEFERRED variant of Foliage_Impostor.
//
// Same card, same atlas sampling, same discard rule — all three come from the
// shared includes (FoliageImpostorVertexStage.glsl, FoliageImpostorSampling.glsl)
// so the two paths cannot drift. The only thing this file owns is the OUTPUT:
// instead of relighting the card from one directional light and compositing
// into SceneColor, it writes the G-Buffer MRT and lets DeferredLightingPass
// shade it like any other opaque surface.
//
// Why this exists (#1225): the forward card is drawn AFTER DeferredLightingPass,
// so it never appeared in GBufferAlbedo / GBufferNormal. Everything derived
// from the G-Buffer therefore treated the canopy as empty sky — SSAO, SSGI and
// SSR got no canopy occluder, and the forest floor was lit like open ground. It
// also meant the canopy saw exactly ONE light plus a flat 0.3 ambient while
// every other surface received the full deferred light list, shadows and IBL.
//
// This is legal because the impostor path was never alpha-BLENDED: the card is
// opaque alpha-TESTED with depth write on and blending disabled by its render
// state, which is exactly the contract an MRT G-Buffer write requires.
//
// Known, deliberate difference from the forward relight: that path had a
// back-lit fallback (`NdotL < 0.01 -> 0.5 * dot(-N, L)`). The G-Buffer carries
// one normal and no two-sided flag, so a sun behind the canopy contributes no
// direct term here and the card is lit by shadows/IBL/probes only — the same
// behaviour Foliage_Instance_GBuffer has always had. Restoring it is a
// DeferredLighting feature (a foliage flag lane), not something to fake here.
// =============================================================================

#type vertex
#version 460 core

// The fragment below reads u_EntityID through InstanceBlock.glsl, so the
// instance index IS forwarded: no OLO_INSTANCE_NO_FORWARD here.
#include "include/FoliageImpostorVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;
// RT5 coverage flag — an MRT output this shader never writes is UNDEFINED in
// that attachment, and undefined there reads as "this pixel has baked GI"
// (issue #865). Write the explicit zero every non-lightmapped writer owes.
layout(location = 5) out vec4 o_GBufferBakedGI;

layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 6) in vec3 v_PrevCardWorld;
layout(location = 7) in float v_Radius;
layout(location = 2) in float v_MeshCoverage; // WORLD-space card radius

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
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
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0; // x=framesPerAxis, y=hemi, z=startDistance, w=transitionBand
    vec4 u_ImpostorParams1;
    vec4 u_MeshParams; // issue #1233 — see FoliageInstanceGeometry.glsl
    vec4 u_MeshViewPos; // see ShaderBindingLayout::FoliageUBO // x=enabled, y=meshRadius, z=parallaxScale, w=unused
};

// u_EntityID rides the per-draw instance SSBO (foliage uploads ONE shared
// entry — OLO_INSTANCE_SINGLE in the vertex stage).
#include "include/InstanceBlock.glsl"
#include "include/FoliageImpostorSampling.glsl"
#include "include/GBufferNormalEncode.glsl"

void main()
{
    ImpostorSample card = SampleImpostorCard();

    // The baked object-space normal, rotated into world space by the instance
    // rotation — the same normal the forward card relights with, handed to
    // DeferredLightingPass instead.
    vec3 worldN = normalize(rotateY(card.LocalNormal, v_Rotation));

    // Matches Foliage_Instance_GBuffer: foliage is diffuse, non-metallic, rough.
    float metallic = 0.0;
    float roughness = 0.9;
    float ao = 1.0;

    o_GBufferAlbedo   = vec4(card.Albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(worldN), roughness, ao);
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0); // lit

    // Camera-motion velocity (impostor has no per-instance prev history).
    vec4 clipCurr = u_ViewProjection * vec4(v_CardWorld, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevCardWorld, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_GBufferVelocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferEntityID = u_EntityID;
    o_GBufferBakedGI = vec4(0.0); // no baked lightmap on this surface (issue #865)
}
