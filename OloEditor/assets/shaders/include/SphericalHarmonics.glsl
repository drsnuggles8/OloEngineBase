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

#endif // SPHERICAL_HARMONICS_GLSL
