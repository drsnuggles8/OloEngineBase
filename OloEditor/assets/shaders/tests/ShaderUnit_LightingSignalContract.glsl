// =============================================================================
// ShaderUnit_LightingSignalContract.glsl
//
// Pins the lighting-signal contract (issue #1336) by CALLING the production
// functions in include/PBRCommon.glsl and include/SphericalHarmonics.glsl —
// the functions PBR_MultiLight{,_Skinned}.glsl and DeferredLightingShared.glsl
// compose every lit surface with — rather than transcribing them. Agreement
// here is agreement for every raster path, because there is one
// implementation.
//
// The expected values live on the CPU side (LightingSignalContractGpuTest.cpp)
// and are derived there INDEPENDENTLY: a Lambertian surface under a uniform
// sky of radiance L reflects kD * albedo * L, whatever route the irradiance
// took to the ladder; a composed pixel is the plain sum of its terms with AO
// on the ambient term alone. A probe that agreed with a transcription of
// itself would prove nothing.
//
// ONE COLUMN PER NAMED CLAIM; the probe is 1 pixel tall. rgb = the value,
// a = 1 so an unwritten column (a = 0 after the clear) cannot pass.
//
//   0  LADDER, LIGHTMAP RUNG — full irradiance E = pi * L, the lightmap's
//        storage, through oloNormalizedIrradiance into the probe helper.
//   1  LADDER, IBL RUNG      — normalized irradiance L, the irradiance cube's
//        storage, straight into the same helper.
//   2  LADDER, SH RUNG       — the radiance projection of the uniform field
//        (the bake's storage), through evaluateSHCosineIrradiance and the
//        conversion. Columns 0-2 must all equal kD * albedo * L: three
//        estimates of ONE quantity that disagreed by pi before #1336.
//   3  SH CPU PARITY         — evaluateSHCosineIrradiance on a directional
//        coefficient set, returned raw for SHBasis::
//        EvaluateCosineConvolvedIrradiance to be compared against.
//   4  EMISSION IS NOT OCCLUDED — ambient visibility 0, emission only.
//   5  DIRECT IS NOT OCCLUDED   — ambient visibility 0, direct + ambient in.
//   6  TRACED INDIRECT IS NOT OCCLUDED — ambient visibility 0, a traced tier's
//        indirect diffuse in.
//   7  TRANSMISSION IS NOT OCCLUDED — ambient visibility 0, transmission in.
//   8  SUPERPOSITION — every term at once with visibility 0.37: the composed
//        pixel is the sum the CPU writes down term by term.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

void main()
{
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/SphericalHarmonics.glsl"

layout(location = 0) out vec4 o_Result;

// THE FIXTURE, mirrored literal for literal in LightingSignalContractGpuTest.cpp.
const vec3 kAlbedo = vec3(0.8, 0.5, 0.2);
const float kRoughness = 0.5;
const vec3 kNormal = vec3(0.0, 0.0, 1.0);
const vec3 kUniformRadiance = vec3(1.2, 0.9, 0.6);

const vec3 kDirectDiffuse = vec3(0.30, 0.20, 0.10);
const vec3 kDirectSpecular = vec3(0.05, 0.06, 0.07);
const vec3 kAmbientDiffuse = vec3(0.11, 0.12, 0.13);
const vec3 kAmbientSpecular = vec3(0.02, 0.03, 0.04);
const vec3 kTracedIndirect = vec3(0.21, 0.17, 0.09);
const vec3 kUnsplitDirect = vec3(0.07, 0.05, 0.03);
const vec3 kTransmitted = vec3(0.015, 0.025, 0.035);
const vec3 kEmissive = vec3(1.5, 0.25, 0.75);

vec3 view()
{
    return normalize(vec3(0.3, 0.0, 1.0));
}

vec3 ladderFromNormalizedIrradiance(vec3 normalizedIrradiance)
{
    return calculateLightProbeAmbient(normalizedIrradiance, kAlbedo, 0.0, kRoughness, kNormal, view());
}

void main()
{
    int column = int(gl_FragCoord.x);
    vec3 result = vec3(0.0);

    OloSurfaceLighting direct = OloSurfaceLighting(kDirectDiffuse, kDirectSpecular);
    OloSurfaceLighting ambient = OloSurfaceLighting(kAmbientDiffuse, kAmbientSpecular);

    if (column == 0)
    {
        result = ladderFromNormalizedIrradiance(oloNormalizedIrradiance(PI * kUniformRadiance));
    }
    else if (column == 1)
    {
        result = ladderFromNormalizedIrradiance(kUniformRadiance);
    }
    else if (column == 2)
    {
        // The radiance projection of a constant field L: c_0 = 4 pi Y00 L, every
        // higher band zero — what LightProbeBaker::ProjectToSH stores for it.
        vec3 coefficients[SH_COEFFICIENT_COUNT];
        for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            coefficients[i] = vec3(0.0);
        coefficients[0] = kUniformRadiance * (4.0 * PI * SH_Y00);
        result = ladderFromNormalizedIrradiance(
            oloNormalizedIrradiance(evaluateSHCosineIrradiance(coefficients, kNormal)));
    }
    else if (column == 3)
    {
        vec3 coefficients[SH_COEFFICIENT_COUNT];
        coefficients[0] = vec3(1.00, 0.80, 0.60);
        coefficients[1] = vec3(0.30, -0.10, 0.05);
        coefficients[2] = vec3(0.40, 0.35, 0.10);
        coefficients[3] = vec3(-0.20, 0.15, 0.25);
        coefficients[4] = vec3(0.05, -0.04, 0.03);
        coefficients[5] = vec3(0.02, 0.06, -0.05);
        coefficients[6] = vec3(-0.08, 0.02, 0.04);
        coefficients[7] = vec3(0.03, -0.02, 0.01);
        coefficients[8] = vec3(0.06, 0.01, -0.03);
        result = evaluateSHCosineIrradiance(coefficients, normalize(vec3(0.3, -0.5, 0.8)));
    }
    else if (column == 4)
    {
        OloSurfaceLighting lighting =
            oloComposeReflectedLighting(oloSurfaceLightingZero(), ambient, 0.0, vec3(0.0));
        result = oloComposeSurfaceRadiance(lighting, vec3(0.0), vec3(0.0), kEmissive);
    }
    else if (column == 5)
    {
        OloSurfaceLighting lighting = oloComposeReflectedLighting(direct, ambient, 0.0, vec3(0.0));
        result = oloComposeSurfaceRadiance(lighting, kUnsplitDirect, vec3(0.0), vec3(0.0));
    }
    else if (column == 6)
    {
        OloSurfaceLighting lighting =
            oloComposeReflectedLighting(oloSurfaceLightingZero(), ambient, 0.0, kTracedIndirect);
        result = oloComposeSurfaceRadiance(lighting, vec3(0.0), vec3(0.0), vec3(0.0));
    }
    else if (column == 7)
    {
        OloSurfaceLighting lighting =
            oloComposeReflectedLighting(oloSurfaceLightingZero(), ambient, 0.0, vec3(0.0));
        result = oloComposeSurfaceRadiance(lighting, vec3(0.0), kTransmitted, vec3(0.0));
    }
    else if (column == 8)
    {
        OloSurfaceLighting lighting = oloComposeReflectedLighting(direct, ambient, 0.37, kTracedIndirect);
        result = oloComposeSurfaceRadiance(lighting, kUnsplitDirect, kTransmitted, kEmissive);
    }

    o_Result = vec4(result, 1.0);
}
