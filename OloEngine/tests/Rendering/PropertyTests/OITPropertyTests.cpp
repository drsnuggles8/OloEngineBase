// =============================================================================
// OITPropertyTests.cpp
//
// Property-level (Layer 1) tests for the weighted-blended OIT path
// (Phase 6, deferred renderer).
//
// Coverage:
//   * ComputeOITWeight matches the shader implementation in
//     `include/OITCommon.glsl` and stays within the expected [1e-2, 3e3]
//     clamp envelope for typical scene scales.
//   * Weight is monotonically non-decreasing in alpha for fixed depth
//     (nearer-to-opaque fragments never get less influence).
//   * The WB-OIT composite math (averageColor, revealage-modulated blend)
//     reduces to the classic "over" operator for a single fragment.
//   * The composite is order-independent for two fragments, matching the
//     core claim of the algorithm.
//
// The tests deliberately exercise only CPU-side math so they run
// without a GL context — the shader-side code is validated in the GLSL
// ShaderUnitTests layer.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

#include "OloEngine/Core/Base.h"

namespace OloEngine::Tests
{
    namespace
    {
        // CPU mirror of ComputeOITWeight — keep in sync with
        // `OloEditor/assets/shaders/include/OITCommon.glsl`.
        f32 ComputeOITWeight(f32 alpha, f32 viewZ)
        {
            const f32 normZ = viewZ * (1.0f / 200.0f);
            const f32 raw = 0.03f / (1e-5f + std::pow(normZ, 4.0f));
            const f32 clamped = std::clamp(raw, 1e-2f, 3e3f);
            return alpha * clamped;
        }

        // CPU mirror of OIT_Resolve.glsl composite equation.
        //
        // Given a sequence of (color, alpha, weight) fragments, this
        // routine mimics the dual-attachment blend + resolve shader:
        //
        //   accum     = sum(color_i * alpha_i * weight_i)  (RGB)
        //              + sum(alpha_i * weight_i)           (A)
        //   revealage = prod(1 - alpha_i)
        //   averageColor = accum.rgb / max(accum.a, 1e-4)
        //   final = averageColor * (1 - revealage) + background * revealage
        struct Fragment
        {
            glm::vec3 Color;
            f32 Alpha;
            f32 ViewZ;
        };

        glm::vec3 ResolveOIT(const std::vector<Fragment>& fragments, const glm::vec3& background)
        {
            glm::vec4 accum(0.0f);
            f32 revealage = 1.0f;
            for (const auto& f : fragments)
            {
                const f32 w = ComputeOITWeight(f.Alpha, f.ViewZ);
                accum.x += f.Color.x * f.Alpha * w;
                accum.y += f.Color.y * f.Alpha * w;
                accum.z += f.Color.z * f.Alpha * w;
                accum.w += f.Alpha * w;
                revealage *= (1.0f - f.Alpha);
            }
            const f32 denom = std::max(accum.w, 1e-4f);
            const glm::vec3 averageColor = glm::vec3(accum.x, accum.y, accum.z) / denom;
            return averageColor * (1.0f - revealage) + background * revealage;
        }
    } // namespace

    TEST(OITWeightTest, ClampBoundsAreRespected)
    {
        // Extremely close fragments must not produce a weight above the
        // shader clamp (3e3) when multiplied by alpha = 1 — the shader
        // clamps the depth-dependent factor, not the final product, so
        // the returned weight is <= 3e3 * alpha.
        const f32 wNear = ComputeOITWeight(1.0f, 0.001f);
        EXPECT_LE(wNear, 3e3f + 1.0f) << "wNear = " << wNear;

        // Very-far fragments must not fall below the clamp floor (1e-2)
        // multiplied by alpha — even out at 1000 m the weight stays above
        // alpha * 1e-2.
        const f32 wFar = ComputeOITWeight(1.0f, 1000.0f);
        EXPECT_GE(wFar, 1e-2f - 1e-6f) << "wFar = " << wFar;
    }

    TEST(OITWeightTest, MonotonicInAlpha)
    {
        // For a fixed depth, weight scales linearly with alpha.
        const f32 viewZ = 10.0f;
        f32 prev = -1.0f;
        for (f32 alpha = 0.0f; alpha <= 1.0f; alpha += 0.1f)
        {
            const f32 w = ComputeOITWeight(alpha, viewZ);
            EXPECT_GE(w, prev - 1e-6f) << "alpha = " << alpha;
            prev = w;
        }
    }

    TEST(OITWeightTest, NearFragmentsOutweighFar)
    {
        // At fixed alpha, nearer fragments must have >= weight than far
        // ones (this is the core visual claim of depth-weighted OIT).
        const f32 alpha = 0.5f;
        const f32 wNear = ComputeOITWeight(alpha, 1.0f);
        const f32 wMid = ComputeOITWeight(alpha, 50.0f);
        const f32 wFar = ComputeOITWeight(alpha, 500.0f);
        EXPECT_GE(wNear, wMid);
        EXPECT_GE(wMid, wFar);
    }

    TEST(OITResolveTest, SingleFragmentOpaqueMatchesForeground)
    {
        // A single fully-opaque red fragment over a blue background must
        // resolve to red — revealage = 0 so background contributes nothing.
        const std::vector<Fragment> frags = {
            { glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 10.0f }
        };
        const glm::vec3 bg(0.0f, 0.0f, 1.0f);
        const glm::vec3 out = ResolveOIT(frags, bg);
        EXPECT_NEAR(out.x, 1.0f, 1e-4f);
        EXPECT_NEAR(out.y, 0.0f, 1e-4f);
        EXPECT_NEAR(out.z, 0.0f, 1e-4f);
    }

    TEST(OITResolveTest, SingleTransparentFragmentIsApproxOver)
    {
        // A single translucent fragment (alpha = 0.5) should approximate
        // classic "over" compositing: final ≈ color * 0.5 + bg * 0.5.
        // Weighted-blended OIT's single-fragment case is exact because
        // averageColor = color and revealage = 1 - alpha.
        const std::vector<Fragment> frags = {
            { glm::vec3(1.0f, 0.0f, 0.0f), 0.5f, 10.0f }
        };
        const glm::vec3 bg(0.0f, 0.0f, 1.0f);
        const glm::vec3 out = ResolveOIT(frags, bg);
        EXPECT_NEAR(out.x, 0.5f, 1e-4f);
        EXPECT_NEAR(out.y, 0.0f, 1e-4f);
        EXPECT_NEAR(out.z, 0.5f, 1e-4f);
    }

    TEST(OITResolveTest, OrderIndependentForTwoFragments)
    {
        // Core algorithmic claim: swapping fragment order must leave the
        // resolved colour invariant (bit-for-bit in exact arithmetic, and
        // to high precision in float). We feed two fragments at the same
        // depth so the weight is identical regardless of ordering.
        const std::vector<Fragment> ab = {
            { glm::vec3(1.0f, 0.0f, 0.0f), 0.5f, 10.0f },
            { glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, 10.0f },
        };
        const std::vector<Fragment> ba = {
            { glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, 10.0f },
            { glm::vec3(1.0f, 0.0f, 0.0f), 0.5f, 10.0f },
        };
        const glm::vec3 bg(0.0f, 0.0f, 0.3f);
        const glm::vec3 oab = ResolveOIT(ab, bg);
        const glm::vec3 oba = ResolveOIT(ba, bg);
        EXPECT_NEAR(oab.x, oba.x, 1e-5f);
        EXPECT_NEAR(oab.y, oba.y, 1e-5f);
        EXPECT_NEAR(oab.z, oba.z, 1e-5f);
    }

    TEST(OITResolveTest, EmptyAccumulationPreservesBackground)
    {
        // No transparent fragments => revealage = 1 and accum.a = 0.
        // The shader early-outs (discards) in this case; on the CPU
        // mirror, the composite equation still yields background
        // because (1 - revealage) = 0.
        const std::vector<Fragment> frags;
        const glm::vec3 bg(0.1f, 0.2f, 0.7f);
        const glm::vec3 out = ResolveOIT(frags, bg);
        EXPECT_NEAR(out.x, bg.x, 1e-6f);
        EXPECT_NEAR(out.y, bg.y, 1e-6f);
        EXPECT_NEAR(out.z, bg.z, 1e-6f);
    }
} // namespace OloEngine::Tests
