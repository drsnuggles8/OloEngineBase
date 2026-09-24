// =============================================================================
// SphericalHarmonics.glsl — L2 SH basis evaluation
// Must match CPU-side constants in OloEngine/Renderer/SphericalHarmonics.h
// =============================================================================

#ifndef SPHERICAL_HARMONICS_GLSL
#define SPHERICAL_HARMONICS_GLSL

#define SH_COEFFICIENT_COUNT 9

// L2 SH basis constants
const float SH_Y00  = 0.282095;   // 1 / (2 * sqrt(pi))
const float SH_Y1n1 = 0.488603;   // sqrt(3) / (2 * sqrt(pi))
const float SH_Y10  = 0.488603;
const float SH_Y11  = 0.488603;
const float SH_Y2n2 = 1.092548;   // sqrt(15) / (2 * sqrt(pi))
const float SH_Y2n1 = 1.092548;
const float SH_Y20  = 0.315392;   // sqrt(5) / (4 * sqrt(pi))
const float SH_Y21  = 1.092548;
const float SH_Y22  = 0.546274;   // sqrt(15) / (4 * sqrt(pi))

// Evaluate all 9 L2 SH basis functions for a direction
void evaluateSHBasis(vec3 dir, out float basis[SH_COEFFICIENT_COUNT])
{
    basis[0] = SH_Y00;
    basis[1] = SH_Y1n1 * dir.y;
    basis[2] = SH_Y10  * dir.z;
    basis[3] = SH_Y11  * dir.x;
    basis[4] = SH_Y2n2 * dir.x * dir.y;
    basis[5] = SH_Y2n1 * dir.y * dir.z;
    basis[6] = SH_Y20  * (3.0 * dir.z * dir.z - 1.0);
    basis[7] = SH_Y21  * dir.x * dir.z;
    basis[8] = SH_Y22  * (dir.x * dir.x - dir.y * dir.y);
}

// Evaluate irradiance from SH coefficients for a given surface normal
// coefficients: array of 9 vec3 values (the .xyz of the SSBO vec4s)
vec3 evaluateSH(vec3 coefficients[SH_COEFFICIENT_COUNT], vec3 normal)
{
    float basis[SH_COEFFICIENT_COUNT];
    evaluateSHBasis(normal, basis);

    vec3 result = vec3(0.0);
    for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        result += coefficients[i] * basis[i];
    }
    return max(result, vec3(0.0));
}

// IRRADIANCE E from RADIANCE-projection coefficients (issue #1336).
//
// The probe bakes store c_i = integral of L * Y_i (no cosine lobe), so
// evaluateSH above reconstructs band-limited RADIANCE. The irradiance a surface
// with normal n receives is the same expansion with each band convolved by the
// clamped cosine (Ramamoorthi & Hanrahan 2001): A0 = pi, A1 = 2pi/3, A2 = pi/4.
// A uniform field L therefore returns pi * L, the full E every other irradiance
// source in the ladder returns — which is what lets the probe volume blend
// baked SH and DDGI (full E) like with like. Mirrored on the CPU by
// SHBasis::EvaluateCosineConvolvedIrradiance.
vec3 evaluateSHCosineIrradiance(vec3 coefficients[SH_COEFFICIENT_COUNT], vec3 normal)
{
    const float A0 = 3.14159265359;
    const float A1 = 2.09439510239; // 2 pi / 3
    const float A2 = 0.78539816340; // pi / 4

    float basis[SH_COEFFICIENT_COUNT];
    evaluateSHBasis(normal, basis);

    vec3 result = coefficients[0] * (A0 * basis[0]);
    for (int i = 1; i < 4; ++i)
        result += coefficients[i] * (A1 * basis[i]);
    for (int i = 4; i < SH_COEFFICIENT_COUNT; ++i)
        result += coefficients[i] * (A2 * basis[i]);
    return max(result, vec3(0.0));
}

#endif // SPHERICAL_HARMONICS_GLSL
