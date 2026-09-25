// =============================================================================
// DepthNormalPrepassFragment.glsl — the fragment stage of the four
// DepthNormalPrepass*.glsl programs (issue #1452).
//
// The forward depth prepass used to write depth only. Screen-space AO now runs
// BETWEEN the prepass and forward colour, so that every forward shader can apply
// it to its ambient term — and AO needs the surface normal. This stage writes
// the normal the colour pass would write to scene attachment 2: the same
// normal-mapped, pore-banded, cornea-tilted, snow-filled normal, through the
// same functions (include/ForwardShadingNormal.glsl, include/SnowLayer.glsl).
//
// ONLY ATTACHMENT 2 IS WRITTEN. CommandDispatch masks every other attachment
// while the forward prepass runs; this stage declares the one output, so no
// attachment is left with a write enabled and no value behind it.
//
// The including file defines OLO_DEPTH_NORMAL_PREPASS_MASK for the glTF MASK
// variants, and puts the Vulkan heap-reader #extension directives at the top of
// its fragment stage, where GLSL requires them (they cannot live in an include).
// =============================================================================

#ifndef DEPTH_NORMAL_PREPASS_FRAGMENT_GLSL
#define DEPTH_NORMAL_PREPASS_FRAGMENT_GLSL

// FIRST, because the sampler declarations below expand its accessor macros on
// the bindless build.
#include "BindlessHeap.glsl"
#include "PBRCommon.glsl"
#include "CameraCommon.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 2) out vec2 o_ViewNormal;

// PBR Material UBO (binding 2) — the full block PBR_MultiLight.glsl declares.
// Every member is kept, including the ones this stage never reads: std140
// places u_MaterialHeapOffsets after all of them, and a shorter prefix would
// read the heap offsets from the wrong bytes.
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;
    vec4 u_EmissiveFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    int u_UseAlbedoMap;
    int u_UseNormalMap;
    int u_UseMetallicRoughnessMap;
    int u_UseAOMap;
    int u_UseEmissiveMap;
    int u_EnableIBL;
    int u_ApplyGammaCorrection;
    float u_AlphaCutoff;
    int u_EnableLightProbes;
    float u_IBLIntensity;
    int u_AlphaMode;
    int u_PBRModel;
    float u_TransmissionFactor;
    float u_IOR;
    float u_ThicknessFactor;
    float u_AttenuationSigmaR;
    float u_AttenuationSigmaG;
    float u_AttenuationSigmaB;
    int u_MaterialKind;
    int u_SkinProfileSlot;
    float u_SkinSpecularTintR;
    float u_SkinSpecularTintG;
    float u_SkinSpecularTintB;
    int u_SkinEvaluationModel;
    vec4 u_SkinTransmitScatter;
    vec4 u_SkinTransmitScaling;
    vec4 u_SkinSpecularLane;
    vec4 u_SkinOralLane;
    vec4 u_SkinOcularCorneaLane;
    vec4 u_SkinOcularIrisLane;
    vec4 u_SkinOcularResponseLane;
    vec4 u_SkinOcularTintLane;
    int u_UseThicknessMap;
    uint u_ThicknessMapHeapOffset;
    float u_SkinThicknessBaseMM;
    float u_SkinDetailStrength;
#if defined(OLO_BINDLESS) || defined(OLO_MATERIAL_VULKAN_HEAP_READER)
    uvec4 u_MaterialHeapOffsets[3];
#endif
};

// The two material maps this stage reads — the albedo for the MASK alpha test,
// the normal map for the normal — on the same three arms PBR_MultiLight.glsl
// uses, so they sample the same descriptor the colour pass does.
#ifdef OLO_MATERIAL_VULKAN_HEAP_READER
#include "DescriptorHeapTextures.glsl"
#define u_AlbedoMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_NormalMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#elif defined(OLO_BINDLESS)
#define OLO_MATERIAL_HEAP_READER 1
#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#define u_NormalMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET)
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap; // TEX_DIFFUSE
layout(binding = 2) uniform sampler2D u_NormalMap; // TEX_NORMAL
#endif

// u_Model, for the ocular surface's optical axis.
#include "InstanceBlock.glsl"
#include "ForwardShadingNormal.glsl"
#include "SnowLayer.glsl"

void main()
{
#ifdef OLO_DEPTH_NORMAL_PREPASS_MASK
    // glTF MASK alpha test — identical to PBR_MultiLight.glsl so the prepass
    // carves the same depth coverage as the colour pass.
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }
#endif

    vec3 N = oloForwardMappedNormal(v_Normal, v_TexCoord, v_WorldPos);

    // The cornea and iris tilt the normal (issue #1244). The albedo half of
    // the result is not needed here, and the normal half does not depend on it.
    if (oloSkinEvaluatesOcularSurface(u_MaterialKind, u_SkinEvaluationModel))
    {
        OloSkinOcular oloOcular = oloSkinOcularApply(vec3(0.0), N, normalize(u_CameraPosition - v_WorldPos),
                                                     u_Model[2].xyz, u_SkinOcularCorneaLane, u_SkinOcularIrisLane,
                                                     u_SkinOcularResponseLane, u_SkinOcularTintLane);
        N = oloOcular.Normal;
    }

    // The snow-FILLED normal, the one the colour pass stores (SnowLayer.glsl).
    float snowWeight = oloSnowLayerCoverage(v_WorldPos + u_RenderOrigin, v_Normal);
    o_ViewNormal = oloForwardViewNormalOutput(u_View, oloSnowLayerFilledNormal(N, snowWeight));
}

#endif // DEPTH_NORMAL_PREPASS_FRAGMENT_GLSL
