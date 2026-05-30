// =============================================================================
// MathCommon.glsl — shared low-level math for the PBR / IBL shader set
//
// Single home for the small numeric primitives that several shaders need.
// Before this file existed they were copy-pasted into every IBL bake shader
// (IBLPrefilter, BRDFLutGeneration, IrradianceConvolution*, …) and drifted:
// some carried the modern bitwise radical inverse, others a 32-iteration
// float loop computing the same thing more slowly and less precisely. See
// GitHub issue #262. Include this header and delete the local copies so the
// optimized forms can never diverge again.
//
// The pipeline cross-compiles GLSL -> SPIR-V -> GLSL; everything here is plain
// arithmetic with no opaque uniforms, so it survives that round-trip.
// =============================================================================

#ifndef MATH_COMMON_GLSL
#define MATH_COMMON_GLSL

#ifndef PI
#define PI 3.14159265359
#endif

// -----------------------------------------------------------------------------
// Integer-power helpers
//
// pow(x, k) for a constant integer k still goes through exp2(k * log2(x)) on
// most hardware — two transcendentals. A multiply chain is a few MULs and, for
// the x in [0, 1] these are always called with (Schlick Fresnel, range
// falloff, OIT weighting), bit-near-identical. The wins that matter are the
// per-frame callers (every lit pixel evaluates Schlick Fresnel at least once).
// -----------------------------------------------------------------------------
float Pow2(float x) { return x * x; }
float Pow3(float x) { return x * x * x; }
float Pow4(float x) { float x2 = x * x; return x2 * x2; }
float Pow5(float x) { float x2 = x * x; return x2 * x2 * x; }

// -----------------------------------------------------------------------------
// Low-discrepancy sampling
// -----------------------------------------------------------------------------

// Van der Corput radical inverse in base 2, computed by reversing the bits of
// `bits` and scaling into [0, 1). This is the branch-free counterpart of the
// old `VanDerCorput(n, 2)` float loop: identical sequence, one integer reversal
// plus a single multiply instead of 32 conditional float iterations.
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // 1 / 2^32
}

// The i-th point of the 2D Hammersley set over N samples.
vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

// -----------------------------------------------------------------------------
// Orthonormal basis
// -----------------------------------------------------------------------------

// Build a right-handed orthonormal basis (t, b, n) around a unit vector n,
// branch-free and without a normalize. Duff, Burgess, Christensen, Cui,
// Disney, Jarosz & Kulla 2017, "Building an Orthonormal Basis, Revisited"
// (JCGT). Replaces the `abs(n.z) < 0.999 ? up=Z : up=X` + 2x cross + normalize
// idiom that the IBL shaders had copy-pasted: same quality, fewer ALU ops, and
// numerically robust at both poles (the old fixed-up trick degenerates as n
// approaches the chosen up axis). The exact tangent differs from the old basis
// — a rotation about n — which is irrelevant to the rotationally-symmetric GGX
// importance integral these feed.
void OrthonormalBasis(vec3 n, out vec3 t, out vec3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0; // copysign(1, n.z); a select, not a branch
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// -----------------------------------------------------------------------------
// GGX importance sampling
// -----------------------------------------------------------------------------

// Importance-sample the GGX/Trowbridge-Reitz NDF around the surface normal N,
// returning a world-space half-vector for the low-discrepancy point Xi.
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Tangent-space half-vector from spherical coordinates.
    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // Tangent-space -> world-space.
    vec3 tangent, bitangent;
    OrthonormalBasis(N, tangent, bitangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

#endif // MATH_COMMON_GLSL
