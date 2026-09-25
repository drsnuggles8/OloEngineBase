// =============================================================================
// AmbientLadder.glsl — the PBR ambient (indirect diffuse + specular) source
// ladder. ONE definition for every consumer (issues #439, #1336):
// PBR_MultiLight and PBR_MultiLight_Skinned (forward), DeferredLightingShared
// (deferred, which used to carry a hand-kept copy of these rungs) and
// PostProcess_SSGI (which needs the diffuse rung it replaces). Same rungs, same
// helpers, same energy split — by construction rather than by review.
//
// Rungs, first match wins:
//   1. baked lightmap   (lightmapSample.a > 0.5 — static receivers only;
//                        dynamic shaders pass vec4(0.0) and skip the rung)
//   2. probe volume     (enableProbes, non-zero probe irradiance)
//   3. environment IBL  (enableIBL)
//   4. flat ambient     (calculateSimpleAmbient's constant)
// IBL *specular* (prefilteredColor + BRDF LUT) is kept on every rung when
// enableIBL is on — the lightmap/probes replace only the diffuse term.
//
// UNITS (issue #1336): the lightmap atlas and the probe volume (baked SH and
// DDGI alike) hand back full irradiance E; the diffuse helper takes NORMALIZED
// irradiance E/pi, the quantity the IBL irradiance cube stores. Each E source
// therefore enters through oloNormalizedIrradiance(), once, at its rung — the
// conversion the lightmap and DDGI rungs used to skip, shading their pixels pi
// times brighter than the reference `albedo / pi * E`.
//
// Include prerequisites (each includer declares these BEFORE this file):
//   - PBRCommon.glsl (calculateLightProbeAmbient, oloNormalizedIrradiance,
//     fresnelSchlickRoughness)
//   - LightProbeSampling.glsl (sampleProbeVolumeIrradiance)
//   - for the evaluateAmbientLadder{,Split} spellings only: the material UBO
//     members u_EnableIBL, u_EnableLightProbes, u_IBLIntensity. An includer
//     without that UBO (a fullscreen pass) defines
//     OLO_AMBIENT_LADDER_EXPLICIT_CONTROLS and calls the ...Ex spelling.
// =============================================================================

#ifndef AMBIENT_LADDER_GLSL
#define AMBIENT_LADDER_GLSL

// The flat fill's constant, in the same units as a rung's normalized
// irradiance: calculateSimpleAmbient is `0.03 * albedo`.
const vec3 OLO_AMBIENT_FLAT_FILL = vec3(0.03);

// THE DIFFUSE RUNG AS DATA (issue #1336). Which normalized irradiance the
// selected rung hands the diffuse helper, at what scale, and whether the
// Fresnel/metallic split applies (the flat fill carries none). Selecting it is
// the ladder's whole decision; the diffuse term is then a pure function of it,
// which is what lets SSGI ask "what did the ladder assume arrives here?" and
// get the answer the lighting pass used, not a second opinion.
struct OloAmbientDiffuseRung
{
    vec3 NormalizedIrradiance; // E / pi (flat fill: OLO_AMBIENT_FLAT_FILL)
    float Scale;               // iblIntensity on an IBL-enabled rung, 1 otherwise
    bool FresnelWeighted;      // false only for the flat fill
};

OloAmbientDiffuseRung oloAmbientFlatFillRung()
{
    return OloAmbientDiffuseRung(OLO_AMBIENT_FLAT_FILL, 1.0, false);
}

OloAmbientDiffuseRung oloSelectAmbientDiffuseRung(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                                                  samplerCube irradianceMap, bool enableIBL,
                                                  bool enableProbes, float iblIntensity)
{
    float iblScale = enableIBL ? iblIntensity : 1.0;
    if (lightmapSample.a > 0.5)
    {
        // Baked lightmap replaces the diffuse ambient term with the same
        // replacement semantics as the probe rung below. The coverage gate is
        // the sample's alpha, NOT the colour: a validly baked pure-black texel
        // (an enclosed surface no indirect light reaches) must keep its baked
        // darkness rather than fall through and glow with sky IBL.
        // Deliberately not gated on enableProbes: baked GI is its own source,
        // and the scene kill switch lives in u_LightmapEnabled.
        return OloAmbientDiffuseRung(oloNormalizedIrradiance(lightmapSample.rgb), iblScale, true);
    }
    if (enableProbes)
    {
        // Issue #632: unified probe sampling — realtime DDGI atlases when a
        // Realtime/Hybrid volume is bound, baked SH otherwise.
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
            return OloAmbientDiffuseRung(oloNormalizedIrradiance(probeIrradiance), iblScale, true);
        // Outside the probe volume: fall back to IBL, or to the flat fill.
    }
    if (enableIBL)
        return OloAmbientDiffuseRung(texture(irradianceMap, N).rgb, iblIntensity, true);
    return oloAmbientFlatFillRung();
}

// The diffuse half of the ambient term for a selected rung — BRDF-weighted
// outgoing radiance, before AO.
vec3 oloAmbientDiffuseFromRung(OloAmbientDiffuseRung rung, vec3 albedo, float metallic, float roughness,
                               vec3 N, vec3 V)
{
    if (!rung.FresnelWeighted)
        return rung.NormalizedIrradiance * albedo * rung.Scale;
    return calculateLightProbeAmbient(rung.NormalizedIrradiance, albedo, metallic, roughness, N, V) * rung.Scale;
}

// The specular half: the caller-resolved prefiltered radiance (global
// prefilter, or the distance-impostor probe blend) through the split-sum LUT.
// Present on every rung when IBL is on, absent when it is off.
vec3 oloAmbientSpecular(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, sampler2D brdfLut,
                        vec3 prefilteredColor)
{
    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec2 envBRDF = texture(brdfLut, vec2(max(dot(N, V), 0.0), roughness)).rg;
    return prefilteredColor * (F * envBRDF.x + envBRDF.y);
}

// THE LADDER, controls passed in (issue #1336) — what the deferred pass calls
// with its DeferredLightingControls lanes, and what the material-UBO spelling
// below forwards to. Diffuse and specular kept apart (issue #1231), so a caller
// that blurs the diffuse ambient for skin can reach them.
OloSurfaceLighting evaluateAmbientLadderSplitEx(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                                                vec3 albedo, float metallic, float roughness,
                                                samplerCube irradianceMap, sampler2D brdfLut,
                                                vec3 prefilteredColor, bool enableIBL, bool enableProbes,
                                                float iblIntensity)
{
    OloAmbientDiffuseRung rung = oloSelectAmbientDiffuseRung(lightmapSample, worldPos, N, V, irradianceMap,
                                                             enableIBL, enableProbes, iblIntensity);
    OloSurfaceLighting ambient;
    ambient.Diffuse = oloAmbientDiffuseFromRung(rung, albedo, metallic, roughness, N, V);
    ambient.Specular = enableIBL ? oloAmbientSpecular(N, V, albedo, metallic, roughness, brdfLut, prefilteredColor) *
                                       iblIntensity
                                 : vec3(0.0);
    return ambient;
}

#ifndef OLO_AMBIENT_LADDER_EXPLICIT_CONTROLS
// The forward spelling: the controls come from the material UBO.
OloSurfaceLighting evaluateAmbientLadderSplit(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                                              vec3 albedo, float metallic, float roughness, float ao,
                                              samplerCube irradianceMap, sampler2D brdfLut,
                                              vec3 prefilteredColor)
{
    return evaluateAmbientLadderSplitEx(lightmapSample, worldPos, N, V, albedo, metallic, roughness,
                                        irradianceMap, brdfLut, prefilteredColor, u_EnableIBL == 1,
                                        u_EnableLightProbes == 1, u_IBLIntensity);
}

// The combined spelling.
vec3 evaluateAmbientLadder(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                           vec3 albedo, float metallic, float roughness, float ao,
                           samplerCube irradianceMap, sampler2D brdfLut,
                           vec3 prefilteredColor)
{
    return oloSurfaceLightingSum(evaluateAmbientLadderSplit(lightmapSample, worldPos, N, V, albedo,
                                                            metallic, roughness, ao,
                                                            irradianceMap, brdfLut, prefilteredColor));
}
#endif

#endif // AMBIENT_LADDER_GLSL
