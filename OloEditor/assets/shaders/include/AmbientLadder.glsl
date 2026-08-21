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

vec3 evaluateAmbientLadder(vec4 lightmapSample, vec3 worldPos, vec3 N, vec3 V,
                           vec3 albedo, float metallic, float roughness, float ao,
                           samplerCube irradianceMap, sampler2D brdfLut,
                           vec3 prefilteredColor)
{
    vec3 ambient;
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
            ambient = calculateCombinedAmbientPrefiltered(lightmapSample.rgb, N, V, albedo,
                                                          metallic, roughness,
                                                          brdfLut, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
        else
        {
            ambient = calculateLightProbeAmbient(lightmapSample.rgb, albedo, metallic, roughness, N, V);
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
            ambient = calculateCombinedAmbientPrefiltered(probeIrradiance, N, V, albedo,
                                                          metallic, roughness,
                                                          brdfLut, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
        else
        {
            // Outside probe volume — fall back to IBL
            ambient = calculateIBLPrefiltered(N, V, albedo, metallic, roughness,
                                              irradianceMap, brdfLut, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
    }
    else if (u_EnableLightProbes == 1)
    {
        // Probes only, no IBL specular
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateLightProbeAmbient(probeIrradiance, albedo, metallic, roughness, N, V);
        }
        else
        {
            ambient = calculateSimpleAmbient(albedo, metallic, ao);
        }
    }
    else if (u_EnableIBL == 1)
    {
        ambient = calculateIBLPrefiltered(N, V, albedo, metallic, roughness,
                                          irradianceMap, brdfLut, prefilteredColor);
        ambient *= u_IBLIntensity;
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }
    return ambient;
}

#endif // AMBIENT_LADDER_GLSL
