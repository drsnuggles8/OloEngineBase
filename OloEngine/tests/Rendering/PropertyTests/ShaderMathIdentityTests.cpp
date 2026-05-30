// =============================================================================
// ShaderMathIdentityTests.cpp
//
// Pure-CPU contract tests that pin the *equivalence claims* behind the shader
// performance audit (GitHub issue #262). Each optimization applied to the GLSL
// shader library is a "smart calculation" swap that must not change results:
//
//   1. Loop-based VanDerCorput(n, 2)  ==  bitwise RadicalInverse_VdC(n)
//      (the same base-2 radical inverse, computed branch-free).
//   2. pow(x, k) for small integer k  ==  a multiply chain Pow2/Pow3/Pow4/Pow5
//      (avoids the exp2/log2 pair; bit-near-identical for the x in [0,1] these
//      are called with).
//   3. The branching `abs(n.z) < 0.999 ? up=Z : up=X` tangent frame is replaced
//      by the branchless Duff et al. 2017 OrthonormalBasis — which must still
//      yield a right-handed orthonormal frame around n at every orientation,
//      including the poles where the old fixed-up trick degenerates.
//
// These mirror the exact arithmetic in assets/shaders/include/MathCommon.glsl.
// They run without a GPU (so they gate every commit in CI), and complement the
// GPU probe tests in PbrPropertyTests.cpp that exercise the compiled GLSL.
// If you change MathCommon.glsl, change the mirrors here in lockstep.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace OloEngine::Tests
{
    namespace
    {
        // --- Mirror of the OLD loop-based VanDerCorput(n, base=2) ----------------
        // Ported verbatim from the form IBLPrefilter.glsl / BRDFLutGeneration.glsl
        // carried before the audit. uint(float(n) / 2.0) truncates toward zero,
        // i.e. floor(n/2) for the small indices (n < 2^24) these are called with.
        f32 LoopVanDerCorput(u32 n)
        {
            f32 invBase = 1.0f / 2.0f;
            f32 result = 0.0f;
            for (u32 i = 0u; i < 32u; ++i)
            {
                if (n > 0u)
                {
                    const f32 denom = std::fmod(static_cast<f32>(n), 2.0f);
                    result += denom * invBase;
                    invBase = invBase / 2.0f;
                    n = static_cast<u32>(static_cast<f32>(n) / 2.0f);
                }
            }
            return result;
        }

        // --- Mirror of the NEW bitwise RadicalInverse_VdC ------------------------
        f32 BitwiseRadicalInverse_VdC(u32 bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<f32>(bits) * 2.3283064365386963e-10f; // 1 / 2^32
        }

        // --- Mirror of the integer-power helpers ---------------------------------
        f32 Pow2(f32 x) { return x * x; }
        f32 Pow3(f32 x) { return x * x * x; }
        f32 Pow4(f32 x) { const f32 x2 = x * x; return x2 * x2; }
        f32 Pow5(f32 x) { const f32 x2 = x * x; return x2 * x2 * x; }

        // High fixed exponents that Water.glsl (specular ^16/^256) and
        // SnowCommon.glsl (sparkle glint ^128) replace pow() with — pure
        // squaring chains.
        f32 Pow16(f32 x) { const f32 a2 = x * x, a4 = a2 * a2, a8 = a4 * a4; return a8 * a8; }
        f32 Pow128(f32 x)
        {
            const f32 a2 = x * x, a4 = a2 * a2, a8 = a4 * a4, a16 = a8 * a8;
            const f32 a32 = a16 * a16, a64 = a32 * a32;
            return a64 * a64;
        }
        f32 Pow256(f32 x) { const f32 a16 = Pow16(x), a64 = a16 * a16 * a16 * a16; return a64 * a64 * a64 * a64; }

        // --- Mirror of the branchless Duff et al. 2017 OrthonormalBasis ----------
        void OrthonormalBasis(const glm::vec3& n, glm::vec3& t, glm::vec3& b)
        {
            const f32 s = n.z >= 0.0f ? 1.0f : -1.0f;
            const f32 a = -1.0f / (s + n.z);
            const f32 c = n.x * n.y * a;
            t = glm::vec3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
            b = glm::vec3(c, s + n.y * n.y * a, -n.y);
        }

        // --- Mirror of ImportanceSampleGGX (uses OrthonormalBasis) ---------------
        glm::vec3 ImportanceSampleGGX(const glm::vec2& Xi, const glm::vec3& N, f32 roughness)
        {
            constexpr f32 kPi = 3.14159265359f;
            const f32 a = roughness * roughness;
            const f32 phi = 2.0f * kPi * Xi.x;
            const f32 cosTheta = std::sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
            const f32 sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
            const glm::vec3 H(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

            glm::vec3 tangent, bitangent;
            OrthonormalBasis(N, tangent, bitangent);
            return glm::normalize(tangent * H.x + bitangent * H.y + N * H.z);
        }

        // A spread of unit normals, deliberately including the ±Z poles and the
        // 0.999 threshold band where the old branching basis switched up-vectors.
        std::vector<glm::vec3> SampleUnitNormals()
        {
            std::vector<glm::vec3> normals;
            normals.push_back({ 0.0f, 0.0f, 1.0f });   // +Z pole
            normals.push_back({ 0.0f, 0.0f, -1.0f });  // -Z pole
            normals.push_back({ 1.0f, 0.0f, 0.0f });
            normals.push_back({ 0.0f, 1.0f, 0.0f });
            normals.push_back(glm::normalize(glm::vec3(0.05f, 0.05f, 0.998f)));  // just inside threshold
            normals.push_back(glm::normalize(glm::vec3(0.05f, 0.05f, -0.998f))); // just inside, south
            constexpr f32 kPi = 3.14159265359f;
            for (int it = 0; it < 8; ++it)
            {
                for (int ip = 0; ip < 12; ++ip)
                {
                    const f32 theta = kPi * (static_cast<f32>(it) + 0.5f) / 8.0f;
                    const f32 phi = 2.0f * kPi * static_cast<f32>(ip) / 12.0f;
                    normals.push_back({ std::sin(theta) * std::cos(phi),
                                        std::sin(theta) * std::sin(phi),
                                        std::cos(theta) });
                }
            }
            return normals;
        }
    } // namespace

    // =========================================================================
    // Claim 1: the bitwise radical inverse reproduces the loop's base-2 Van der
    // Corput sequence exactly. Every reversed index < 4096 lands on a dyadic
    // fraction with ≤ 12 significant mantissa bits, so the two are bit-identical
    // here — but we keep a tiny tolerance to document intent rather than ULPs.
    // =========================================================================
    TEST(ShaderMathIdentityTest, BitwiseRadicalInverseMatchesLoopVanDerCorput)
    {
        constexpr f32 kTol = 1e-6f;
        for (u32 i = 0u; i < 4096u; ++i)
        {
            const f32 loop = LoopVanDerCorput(i);
            const f32 bitwise = BitwiseRadicalInverse_VdC(i);
            EXPECT_NEAR(loop, bitwise, kTol) << "VdC mismatch at i=" << i;
        }
    }

    // The radical inverse must stay inside [0, 1) for every input.
    TEST(ShaderMathIdentityTest, RadicalInverseStaysInUnitInterval)
    {
        for (u32 i = 0u; i < 100000u; i += 7u)
        {
            const f32 v = BitwiseRadicalInverse_VdC(i);
            EXPECT_GE(v, 0.0f) << "at i=" << i;
            EXPECT_LT(v, 1.0f) << "at i=" << i;
        }
    }

    // =========================================================================
    // Claim 2: the multiply chains equal pow() for their exponents. The domain
    // that matters is [0,1] (Schlick Fresnel = (1-cos)^5, range falloff =
    // (d/r)^4); we also sweep > 1 because OIT weighting feeds normZ > 1 for
    // fragments beyond OIT_DEPTH_SCALE.
    // =========================================================================
    TEST(ShaderMathIdentityTest, PowHelpersMatchStdPowInUnitInterval)
    {
        constexpr f32 kTol = 2e-6f;
        for (int k = 0; k <= 1000; ++k)
        {
            const f32 x = static_cast<f32>(k) / 1000.0f;
            EXPECT_NEAR(Pow2(x), std::pow(x, 2.0f), kTol) << "Pow2 at x=" << x;
            EXPECT_NEAR(Pow3(x), std::pow(x, 3.0f), kTol) << "Pow3 at x=" << x;
            EXPECT_NEAR(Pow4(x), std::pow(x, 4.0f), kTol) << "Pow4 at x=" << x;
            EXPECT_NEAR(Pow5(x), std::pow(x, 5.0f), kTol) << "Pow5 at x=" << x;
        }
    }

    TEST(ShaderMathIdentityTest, Pow4MatchesStdPowBeyondUnitInterval)
    {
        // OIT weighting: normZ = viewZ / OIT_DEPTH_SCALE can exceed 1. Use a
        // relative tolerance since the magnitudes grow large (x up to 10 -> 1e4).
        for (int k = 0; k <= 1000; ++k)
        {
            const f32 x = static_cast<f32>(k) / 100.0f; // 0 .. 10
            const f32 mul = Pow4(x);
            const f32 ref = std::pow(x, 4.0f);
            EXPECT_NEAR(mul, ref, std::max(1e-4f, std::abs(ref) * 1e-5f)) << "Pow4 at x=" << x;
        }
    }

    // The high fixed exponents (Water specular ^16/^256, snow glint ^128) become
    // squaring chains. All inputs are in [0,1], where the result stays in [0,1],
    // so an absolute tolerance is appropriate. Squaring is in fact *more* precise
    // than pow()'s exp2(k·log2(x)); the tolerance just documents intent.
    TEST(ShaderMathIdentityTest, SquaringChainsMatchStdPowForHighExponents)
    {
        constexpr f32 kTol = 1e-5f;
        for (int k = 0; k <= 1000; ++k)
        {
            const f32 x = static_cast<f32>(k) / 1000.0f;
            EXPECT_NEAR(Pow16(x), std::pow(x, 16.0f), kTol) << "Pow16 at x=" << x;
            EXPECT_NEAR(Pow128(x), std::pow(x, 128.0f), kTol) << "Pow128 at x=" << x;
            EXPECT_NEAR(Pow256(x), std::pow(x, 256.0f), kTol) << "Pow256 at x=" << x;
        }
    }

    // =========================================================================
    // Claim 3a: OrthonormalBasis yields a right-handed orthonormal frame around
    // n for every orientation, including the poles. This is what licenses
    // replacing the branching fixed-up construction in the IBL shaders.
    // =========================================================================
    TEST(ShaderMathIdentityTest, OrthonormalBasisIsOrthonormalAndRightHanded)
    {
        constexpr f32 kTol = 1e-5f;
        for (const glm::vec3& n : SampleUnitNormals())
        {
            glm::vec3 t, b;
            OrthonormalBasis(n, t, b);

            EXPECT_NEAR(glm::length(t), 1.0f, kTol) << "t not unit for n=("
                << n.x << "," << n.y << "," << n.z << ")";
            EXPECT_NEAR(glm::length(b), 1.0f, kTol) << "b not unit";

            EXPECT_NEAR(glm::dot(t, n), 0.0f, kTol) << "t not perpendicular to n";
            EXPECT_NEAR(glm::dot(b, n), 0.0f, kTol) << "b not perpendicular to n";
            EXPECT_NEAR(glm::dot(t, b), 0.0f, kTol) << "t not perpendicular to b";

            // Right-handed: t × b == n.
            const glm::vec3 cross = glm::cross(t, b);
            EXPECT_NEAR(cross.x, n.x, kTol) << "t x b != n (x)";
            EXPECT_NEAR(cross.y, n.y, kTol) << "t x b != n (y)";
            EXPECT_NEAR(cross.z, n.z, kTol) << "t x b != n (z)";
        }
    }

    // No NaN/Inf must escape OrthonormalBasis at the singular -Z pole, where the
    // s = copysign(1, n.z) sign flip keeps (s + n.z) away from zero.
    TEST(ShaderMathIdentityTest, OrthonormalBasisFiniteAtPoles)
    {
        for (const glm::vec3& n : { glm::vec3(0, 0, 1), glm::vec3(0, 0, -1) })
        {
            glm::vec3 t, b;
            OrthonormalBasis(n, t, b);
            for (int c = 0; c < 3; ++c)
            {
                EXPECT_TRUE(std::isfinite(t[c])) << "t not finite at pole";
                EXPECT_TRUE(std::isfinite(b[c])) << "b not finite at pole";
            }
        }
    }

    // =========================================================================
    // Claim 3b: ImportanceSampleGGX, built on the new basis, returns a unit
    // half-vector in the hemisphere of N (dot >= 0). The GGX integral the bake
    // shaders compute is rotationally symmetric about N, so the exact tangent
    // (which differs from the old basis) does not change the converged result —
    // only that the sampled directions remain valid is testable here.
    // =========================================================================
    TEST(ShaderMathIdentityTest, ImportanceSampleGGXProducesUnitHemisphereVectors)
    {
        constexpr f32 kUnitTol = 1e-4f;
        const std::vector<glm::vec3> normals = SampleUnitNormals();
        const f32 roughnesses[] = { 0.05f, 0.25f, 0.5f, 0.8f, 1.0f };
        for (const glm::vec3& N : normals)
        {
            for (const f32 r : roughnesses)
            {
                for (u32 i = 0u; i < 64u; ++i)
                {
                    const glm::vec2 Xi(static_cast<f32>(i) / 64.0f, BitwiseRadicalInverse_VdC(i));
                    const glm::vec3 H = ImportanceSampleGGX(Xi, N, r);
                    EXPECT_NEAR(glm::length(H), 1.0f, kUnitTol) << "non-unit H";
                    EXPECT_GE(glm::dot(H, N), -kUnitTol) << "H below N's hemisphere";
                }
            }
        }
    }

} // namespace OloEngine::Tests
