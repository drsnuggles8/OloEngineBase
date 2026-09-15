// =============================================================================
// Foliage_Instance_GBuffer.glsl — Deferred G-Buffer variant of
// Foliage_Instance.glsl. Same instanced VS (wind + fade + rotation); FS writes
// material data into the 4-RT G-Buffer so foliage participates in the deferred
// lighting composite. Alpha-tested cutouts are expressed as hard `discard`
// (G-Buffer has no alpha blending).
//
// `emissive.a` carries the packed G-Buffer material flags. It used to be a
// literal 0.0 — lit, Generic, Legacy closure. Since issue #1234 a layer that
// authored a LEAF MATERIAL encodes MaterialKind::Foliage and its leaf-profile
// slot through oloEncodeGBufferPbrFlagsEx instead, and parks the pixel's
// THICKNESS in RT5's red channel, so DeferredLightingShared can evaluate the
// transmission lobe. A layer that did not stays byte-identical to before.
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
    // Leaf material (issue #1234) — see ShaderBindingLayout::FoliageUBO.
    vec4 u_LeafSurface;   // x=roughness y=normalStrength z=thicknessScale w=mapFlags
    vec4 u_LeafTransmit;  // rgb=tint*strength w=strength (0 == not a leaf material)
    vec4 u_LeafLobe;      // x=distortion y=power z=wrap w=environment scale
    vec4 u_LeafIds;       // x = leaf-profile slot for the deferred lighting pass
};

#include "include/FoliageInstanceGeometry.glsl"

// Mirror the vertex-stage ModelMatrices block so the entity-ID picking
// slot is available in the fragment stage. SPIR-V link validation rejects
// mismatched layouts so the padding fields stay identical.
#include "include/InstanceBlock.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
// Leaf maps (issue #1234). TEX_METALLIC carries THICKNESS: foliage is never
// metallic, so the slot is definitionally free on this surface, and the engine
// already repurposes a semantic slot per shader this way (PBR_MultiLight's
// u_MetallicRoughnessMap sits on TEX_SPECULAR).
#define u_LeafNormalMap OLO_HEAP_TEX_2D(2)     // TEX_NORMAL
#define u_LeafRoughnessMap OLO_HEAP_TEX_2D(6)  // TEX_ROUGHNESS
#define u_LeafThicknessMap OLO_HEAP_TEX_2D(7)  // TEX_METALLIC (repurposed)
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
layout(binding = 2) uniform sampler2D u_LeafNormalMap;     // TEX_NORMAL
layout(binding = 6) uniform sampler2D u_LeafRoughnessMap;  // TEX_ROUGHNESS
layout(binding = 7) uniform sampler2D u_LeafThicknessMap;  // TEX_METALLIC (repurposed: thickness)
#endif

// The shared vegetation material. PBRCommon first: FoliageSurface's G-Buffer
// flag encoding and the kind constants live there.
#include "include/PBRCommon.glsl"
#define OLO_FOLIAGE_SURFACE_SAMPLING 1
#include "include/FoliageSurface.glsl"

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

    // THE SURFACE, from the shared evaluation (issue #1234). Roughness used to
    // be a hard-coded 0.9 here and the normal the raw interpolated one — the
    // "hardcoded deferred surface attributes" the issue's first criterion
    // replaces. Both now come from oloFoliageSampleSurface, which the FORWARD
    // program calls with the same arguments, so the two paths cannot mean
    // different things by "this leaf".
    vec3 V = normalize(u_CameraPosition - v_WorldPos);
    OloFoliageSurface leaf = oloFoliageSampleSurface(v_WorldPos, v_Normal, v_TexCoord, V, v_Color, texColor);

    // Foliage is never metallic and carries no baked AO map.
    float metallic = 0.0;
    float ao = 1.0;

    o_GBufferAlbedo   = vec4(leaf.Albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(leaf.Normal), leaf.Roughness, ao);

    // The flags lane. OLO_PBR_MODEL_LEGACY is not a choice so much as the
    // status quo preserved: this shader wrote a literal 0.0 lane before #1234,
    // which decodes to exactly Legacy, and moving foliage to another closure
    // would have changed every existing foliage pixel under cover of a
    // transmission feature.
    bool isLeaf = oloLeafEnabled();
    float flags = isLeaf
                      ? oloEncodeGBufferPbrFlagsEx(OLO_PBR_MODEL_LEGACY, OLO_MATERIAL_KIND_FOLIAGE,
                                                   int(u_LeafIds.x + 0.5))
                      : 0.0;
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, flags); // lit; rgb = no emission

    // Camera + wind-reprojection velocity. v_PrevWorldPos already includes the
    // prev-frame wind displacement (evaluated at u_PrevTime in the VS).
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_GBufferVelocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferEntityID = u_EntityID;

    // RT5 — the baked-lightmap target, and the THICKNESS LANE (issue #1234).
    //
    // WHY HERE, AND WHY IT IS SAFE. The transmission lobe needs one per-pixel
    // scalar in the deferred path and the G-Buffer had no spare channel: RT0.a
    // is metallic, RT1.zw are roughness and AO, RT2.rgb is emission that ten
    // ReSTIR shaders read. RT5 is different — its ALPHA is a coverage flag, and
    // every reader of the target already gates on it (there are exactly two:
    // DeferredLighting.glsl and DeferredLighting_MSAA.glsl). Foliage has always
    // written coverage 0 here, so putting thickness in .r and leaving .a at 0
    // changes NOTHING for any existing reader, on any pixel, in any mode: a
    // coverage-0 pixel's rgb is already ignored. The foliage-kind test in the
    // lighting pass is what turns it back into a number.
    //
    // 0 when this layer is not a leaf material, so the lane means "does not
    // transmit" rather than "uninitialised".
    o_GBufferBakedGI = vec4(isLeaf ? leaf.Thickness : 0.0, 0.0, 0.0, 0.0);
}
