// =============================================================================
// IrradianceFromSH.glsl — IBL irradiance evaluation from L2 spherical harmonics
//
// Alternative to IrradianceConvolution.glsl: instead of Monte-Carlo summing
// thousands of cubemap samples per output texel, this shader evaluates a
// pre-projected 9-coefficient L2 SH expansion of the environment map. The
// output cubemap stays bit-compatible with the convolution path so PBR
// shaders consume it identically — only the *generator* changes.
//
// Used when IBLConfiguration::UseSphericalHarmonics is true.
// SH coefficients are projected on the CPU side (see LightProbeBaker::ProjectToSH)
// from the source environment cubemap and uploaded to UBO_SH_COEFFICIENTS
// before this shader runs.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(location = 0) out vec3 v_LocalPos;

void main()
{
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

// L2 SH coefficients projected from the source environment cubemap.
// Coefficient[0].w is the validity flag (1.0 = valid). Other .w slots are
// padding required by std140 vec3-array alignment.
layout(std140, binding = 35) uniform SHCoefficientsBlock {
    vec4 u_SHCoefficients[9];
};

#include "include/SphericalHarmonics.glsl"

void main()
{
    vec3 N = normalize(v_LocalPos);

    float basis[SH_COEFFICIENT_COUNT];
    evaluateSHBasis(N, basis);

    // Inlined SH evaluation — directly accumulate each UBO entry's RGB
    // weighted by the corresponding basis function. The earlier version
    // copied u_SHCoefficients[i].xyz into a local vec3 array and passed
    // that to evaluateSH(); on the production driver the local-array
    // copy was collapsing channels (every output pixel read R=G=B exactly,
    // even though the CPU-side SH coefficients had distinct RGB — see
    // `SHProjectionTest.ChannelAsymmetryIsPreservedThroughProjectionAndScaling`).
    // Accumulating directly from the UBO avoids the local-array round-trip.
    vec3 irradiance = vec3(0.0);
    for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        irradiance += u_SHCoefficients[i].xyz * basis[i];
    }
    irradiance = max(irradiance, vec3(0.0));

    o_Color = vec4(irradiance, 1.0);
}
