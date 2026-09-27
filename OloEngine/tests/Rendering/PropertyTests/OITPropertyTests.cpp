// =============================================================================
// OITPropertyTests.cpp
//
// Property-level (Layer 1) tests for the weighted-blended OIT path
// (deferred renderer).
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
//   * The accumulation survives in the target's STORAGE format (issue
//     #1468): the twin rounds every blend add through the accumulator's
//     format, so a stack that overflows RGBA16F to inf -- and resolves
//     inf / inf = NaN, black -- is visible here, and RGBA32F is pinned as the
//     format that holds it. Text guards tie the twin's constants to the shaders.
//
// The tests deliberately exercise only CPU-side math so they run
// without a GL context — the shader-side code is validated in the GLSL
// ShaderUnitTests layer.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBlendState.h"

#include "Rendering/MockRendererAPI.h"
#include "Rendering/ReSTIR/ShaderSourceScan.h"

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

        // The resolve's division guard, as OIT_Resolve.glsl spells it (pinned by
        // OITResolveTest.TheTwinMatchesTheShaders).
        constexpr f32 kResolveDenominatorFloor = 1e-20f;

        // How the accumulation target stores each blended sum. The blend unit
        // adds in (at least) fp32 and writes the result back in the target's
        // format, so every add is rounded to that format.
        [[nodiscard]] f32 StoreAs(RGResourceFormat format, f32 value)
        {
            if (format == RGResourceFormat::RGBA16Float)
                return glm::unpackHalf1x16(glm::packHalf1x16(value));
            return value;
        }

        glm::vec3 ResolveOIT(const std::vector<Fragment>& fragments, const glm::vec3& background,
                             RGResourceFormat accumFormat = kOITAccumFormat)
        {
            glm::vec4 accum(0.0f);
            f32 revealage = 1.0f;
            for (const auto& f : fragments)
            {
                const f32 w = ComputeOITWeight(f.Alpha, f.ViewZ);
                accum.x = StoreAs(accumFormat, accum.x + f.Color.x * f.Alpha * w);
                accum.y = StoreAs(accumFormat, accum.y + f.Color.y * f.Alpha * w);
                accum.z = StoreAs(accumFormat, accum.z + f.Color.z * f.Alpha * w);
                accum.w = StoreAs(accumFormat, accum.w + f.Alpha * w);
                revealage *= (1.0f - f.Alpha);
            }
            const f32 denom = std::max(accum.w, kResolveDenominatorFloor);
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

    // Issue #1468. About 120 layers of alpha 0.9 at 10.6 m (the particle square
    // in DecalOITScene at 600 particles/s) sum alpha^2 * weight to ~1.6e5, past
    // RGBA16F's 65504: the accumulator read inf, the resolve's inf / inf was NaN
    // and the square composited black. Resolved through the production format
    // the stack is the colour it is made of.
    TEST(OITResolveTest, ADenseNearStackStaysFiniteInTheAccumulatorFormat)
    {
        const glm::vec3 red(1.0f, 0.05f, 0.05f);
        const std::vector<Fragment> stack(120, Fragment{ red, 0.9f, 10.57f });
        const glm::vec3 bg(0.0f, 0.0f, 1.0f);

        const glm::vec3 half = ResolveOIT(stack, bg, RGResourceFormat::RGBA16Float);
        EXPECT_FALSE(std::isfinite(half.x) && std::isfinite(half.y) && std::isfinite(half.z))
            << "negative control: RGBA16F no longer overflows on this stack, so the test proves nothing";

        const glm::vec3 out = ResolveOIT(stack, bg);
        ASSERT_TRUE(std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z))
            << "the production accumulator overflowed on 120 near layers";
        EXPECT_NEAR(out.x, red.x, 1e-3f);
        EXPECT_NEAR(out.y, red.y, 1e-3f);
        EXPECT_NEAR(out.z, red.z, 1e-3f);
    }

    // One layer of HDR colour overflowed RGBA16F on its own: 100 * 0.9 * 0.9 * 3e3
    // is 2.4e5. Emissive particles reach that from YAML, Lua or an HDR texture.
    TEST(OITResolveTest, OneHdrLayerStaysFiniteInTheAccumulatorFormat)
    {
        const std::vector<Fragment> hdr = { { glm::vec3(100.0f, 50.0f, 10.0f), 0.9f, 1.0f } };
        const glm::vec3 bg(0.0f);
        const glm::vec3 half = ResolveOIT(hdr, bg, RGResourceFormat::RGBA16Float);
        EXPECT_FALSE(std::isfinite(half.x)) << "negative control: one HDR layer no longer overflows RGBA16F";

        const glm::vec3 out = ResolveOIT(hdr, bg);
        ASSERT_TRUE(std::isfinite(out.x));
        EXPECT_NEAR(out.x, 90.0f, 1e-2f); // 100 * (1 - revealage), revealage = 0.1
        EXPECT_NEAR(out.y, 45.0f, 1e-2f);
    }

    // The resolve's division guard was 1e-4, larger than a faint far layer's own
    // alpha * weight (a white a=0.05 layer at 300 m sums to 2.5e-5), so the
    // average colour came out a quarter of the layer's colour. A single layer's
    // average colour IS its colour.
    TEST(OITResolveTest, AFaintFarLayerResolvesToItsOwnColour)
    {
        const std::vector<Fragment> faint = { { glm::vec3(1.0f), 0.05f, 300.0f } };
        const glm::vec3 bg(0.0f);
        const glm::vec3 out = ResolveOIT(faint, bg);
        EXPECT_NEAR(out.x, 0.05f, 1e-5f) << "the division guard is darkening a faint layer";
    }

    // Issue #1468. OIT runs only where the device can run it: blend the RGBA32F
    // accumulator and give the two targets different blend factors, both
    // optional on Vulkan. Without them the frame runs no OIT at all, rather than
    // recording a blend state the device cannot honour.
    TEST(OITResolveTest, OITRunsOnlyWhereTheDeviceCanRunIt)
    {
        Testing::MockRendererAPI api;
        EXPECT_TRUE(WeightedBlendedOITIsActive(true, api));
        EXPECT_FALSE(WeightedBlendedOITIsActive(false, api));
        api.SetSupportsWeightedBlendedOIT(false);
        EXPECT_FALSE(WeightedBlendedOITIsActive(true, api)) << "OIT ran on a device that cannot run it";
        EXPECT_FALSE(WeightedBlendedOITIsActive(false, api));
    }

    TEST(OITResolveTest, TheTwinMatchesTheShaders)
    {
        using namespace ShaderScan;
        const std::string common = ReadTextFile(ResolveShaderPath("include/OITCommon.glsl"));
        const std::string resolve = ReadTextFile(ResolveShaderPath("OIT_Resolve.glsl"));
        ASSERT_FALSE(common.empty());
        ASSERT_FALSE(resolve.empty());

        const auto count = [](const std::string& haystack, std::string_view needle)
        {
            sizet n = 0;
            for (sizet at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
                ++n;
            return n;
        };
        EXPECT_EQ(count(common, "#define OIT_DEPTH_SCALE 200.0"), 1u);
        EXPECT_EQ(count(common, "clamp(0.03 / (1e-5 + normZ2 * normZ2), 1e-2, 3e3)"), 1u);
        EXPECT_EQ(count(resolve, "max(accum.a, 1e-20)"), 1u) << "kResolveDenominatorFloor no longer mirrors the shader";
        EXPECT_EQ(count(resolve, "max(accum.a,"), 1u);
        EXPECT_EQ(kOITAccumFormat, RGResourceFormat::RGBA32Float)
            << "the accumulator must be 32-bit float; RGBA16F overflows on ~22 near layers (issue #1468)";
    }
} // namespace OloEngine::Tests
