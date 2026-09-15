// =============================================================================
// AmbientLadder.glsl — the forward PBR ambient (indirect diffuse + specular)
// source ladder, shared by PBR_MultiLight and PBR_MultiLight_Skinned (issue
// #439). One definition so the static (lightmapped) and dynamic (probe-lit)
// forward paths cannot drift structurally: same rungs, same helpers, same
// energy-split semantics.
//
// Rungs, first match wins:
//   1. baked lightmap   (lightmapSample.a > 0.5 — static receivers only;
//                        dynamic shaders pass vec4(0.0) and skip the rung)
//   2. probe volume     (u_EnableLightProbes, non-zero probe irradiance)
//   3. environment IBL  (u_EnableIBL)
//   4. flat ambient     (calculateSimpleAmbient)
// IBL *specular* (prefilteredColor + BRDF LUT) is kept on rungs 1–2 when
// u_EnableIBL is on — the lightmap/probes replace only the diffuse term.
//
// UNITS CAVEAT: the lightmap atlas stores irradiance E; the baked-SH probe
// path reconstructs band-limited RADIANCE (an up-to-π underestimate the
// legacy cubemap bake established — deliberately preserved and numerically
// pinned by LightProbePathTracedBakeTest, see LightProbeBaker.cpp). The
// ladder unifies the STRUCTURE of the two paths, not yet the two sources'
// units; a probe-lit dynamic object can read up to ~π darker than the
// lightmapped floor it stands on.
//
// Include prerequisites (each includer declares these BEFORE this file):
//   - PBRCommon.glsl (calculateCombinedAmbientPrefiltered,
//     calculateLightProbeAmbient, calculateIBLPrefiltered,
//     calculateSimpleAmbient)
//   - LightProbeSampling.glsl (sampleProbeVolumeIrradiance)
//   - the material UBO members u_EnableIBL, u_EnableLightProbes,
//     u_IBLIntensity
// =============================================================================

#ifndef AMBIENT_LADDER_GLSL
#define AMBIENT_LADDER_GLSL

// The REAL body (issue #1231): every rung keeps its diffuse and specular halves
// apart, so a caller that blurs the diffuse ambient for skin can reach them.
// evaluateAmbientLadder below sums it and is what every existing caller uses.
//
// The rung STRUCTURE is untouched -- same order, same gates, same helpers. Only
// the helper spellings changed, each to its `...Split` twin, which are
// themselves wrappers' bodies in PBRCommon.glsl rather than second copies.
OloSurfaceLighting evaluateAmbientLadderSplit(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                                              vec3 albedo, float metallic, float roughness, float ao,
                                              samplerCube irradianceMap, sampler2D brdfLut,
                                              vec3 prefilteredColor)
{
    OloSurfaceLighting ambient;
    if (lightmapSample.a > 0.5)
    {
        // Baked lightmap replaces the diffuse ambient term with the same
        // replacement semantics as the probe rung below. The coverage gate is
        // the sample's alpha, NOT the colour: a validly baked pure-black texel
        // (an enclosed surface no indirect light reaches) must keep its baked
        // darkness rather than fall through and glow with sky IBL.
        // Deliberately not gated on u_EnableLightProbes: baked GI is its own
        // source, and the scene kill switch lives in u_LightmapEnabled.
        if (u_EnableIBL == 1)
        {
            ambient = calculateCombinedAmbientPrefilteredSplit(lightmapSample.rgb, N, V, albedo,
                                                               metallic, roughness,
                                                               brdfLut, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(u_IBLIntensity));
        }
        else
        {
            ambient = calculateLightProbeAmbientSplit(lightmapSample.rgb, albedo, metallic, roughness, N, V);
        }
    }
    else if (u_EnableLightProbes == 1 && u_EnableIBL == 1)
    {
        // Combined: probe diffuse + IBL specular. Issue #632: unified probe
        // sampling — realtime DDGI atlases when a Realtime/Hybrid volume is
        // bound, baked SH otherwise.
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateCombinedAmbientPrefilteredSplit(probeIrradiance, N, V, albedo,
                                                               metallic, roughness,
                                                               brdfLut, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(u_IBLIntensity));
        }
        else
        {
            // Outside probe volume — fall back to IBL
            ambient = calculateIBLPrefilteredSplit(N, V, albedo, metallic, roughness,
                                                   irradianceMap, brdfLut, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(u_IBLIntensity));
        }
    }
    else if (u_EnableLightProbes == 1)
    {
        // Probes only, no IBL specular
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateLightProbeAmbientSplit(probeIrradiance, albedo, metallic, roughness, N, V);
        }
        else
        {
            ambient = calculateSimpleAmbientSplit(albedo, metallic, ao);
        }
    }
    else if (u_EnableIBL == 1)
    {
        ambient = calculateIBLPrefilteredSplit(N, V, albedo, metallic, roughness,
                                               irradianceMap, brdfLut, prefilteredColor);
        ambient = oloSurfaceLightingScale(ambient, vec3(u_IBLIntensity));
    }
    else
    {
        ambient = calculateSimpleAmbientSplit(albedo, metallic, ao);
    }
    return ambient;
}

// The combined spelling every existing caller uses. Same rungs, same values:
// each `...Split` helper is the body of the vec3 helper it replaced, and the
// scale-then-sum here is the same per-component product as the old
// `ambient *= u_IBLIntensity` followed by the caller's addition.
vec3 evaluateAmbientLadder(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                           vec3 albedo, float metallic, float roughness, float ao,
                           samplerCube irradianceMap, sampler2D brdfLut,
                           vec3 prefilteredColor)
{
    return oloSurfaceLightingSum(evaluateAmbientLadderSplit(lightmapSample, worldPos, N, V, albedo,
                                                            metallic, roughness, ao,
                                                            irradianceMap, brdfLut, prefilteredColor));
}

#endif // AMBIENT_LADDER_GLSL
