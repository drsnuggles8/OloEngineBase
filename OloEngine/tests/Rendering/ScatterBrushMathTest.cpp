// =============================================================================
// ScatterBrushMathTest.cpp
//
// Pins the pure-CPU math used by the editor scatter brush
// (`OloEditor/src/Panels/InstanceScatterBrushPanel.cpp`):
//
//   - Slope filter: `dot(surfaceNormal, vec3(0, 1, 0)) >= minDot` decides
//     whether the brush's centre-frame deposit fires. A drift in the
//     threshold semantics here would produce silent scatter coverage gaps
//     near slope cut-offs.
//
//   - Variant index normalisation: variant `i` of `N` encodes as
//     `Custom = i / max(1, N - 1)` so a shader can decode via
//     `idx = i32(Custom * (N - 1))`. The single-variant case (N = 1) must
//     produce `Custom = 0` (not NaN from divide-by-zero), and the last
//     variant must produce `Custom = 1.0` (not slightly < 1 from float
//     rounding).
//
// These tests live in the engine test target rather than the editor
// because the math is pure: same C++23, same GLM types, no editor / ImGui
// dependency.
//
// Classification: L1 / shaderpipe-adjacent (pure CPU).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        // Mirror of the inline math in
        // `InstanceScatterBrushPanel::DepositStrokeTick()`. If this drifts
        // from the panel's logic, the test stops being meaningful — update
        // both together.
        bool SlopeFilterPasses(const glm::vec3& surfaceNormal, f32 minDot)
        {
            if (minDot <= 0.0f)
                return true; // 0 disables the filter
            return glm::dot(surfaceNormal, glm::vec3(0.0f, 1.0f, 0.0f)) >= minDot;
        }

        f32 VariantToCustom(i32 variantIndex, i32 variantCount)
        {
            const i32 clamped = std::clamp(variantIndex, 0, std::max(0, variantCount - 1));
            return (variantCount > 1) ? static_cast<f32>(clamped) / static_cast<f32>(variantCount - 1)
                                       : 0.0f;
        }
    } // namespace

    TEST(ScatterBrushSlopeFilter, FlatGroundPasses)
    {
        // Up-pointing normal trivially passes for any threshold ≤ 1.
        const glm::vec3 flat{ 0.0f, 1.0f, 0.0f };
        EXPECT_TRUE(SlopeFilterPasses(flat, 0.0f));
        EXPECT_TRUE(SlopeFilterPasses(flat, 0.5f));
        EXPECT_TRUE(SlopeFilterPasses(flat, 0.999f));
    }

    TEST(ScatterBrushSlopeFilter, FortyFiveDegreeAtDot07RoughlyAtThreshold)
    {
        // 45° slope: normal at (sin45, cos45, 0). cos45 ≈ 0.7071. The
        // default 0.71 threshold is just above cos(45°), so 45° slopes
        // should fail and gentler slopes should pass — confirming the
        // "≥ 45°" intent of the default in the inspector tooltip.
        const f32 cos45 = std::cos(glm::radians(45.0f));
        const glm::vec3 fortyFiveSlope{ std::sin(glm::radians(45.0f)), cos45, 0.0f };
        EXPECT_FALSE(SlopeFilterPasses(fortyFiveSlope, 0.71f));

        const f32 cos30 = std::cos(glm::radians(30.0f));
        const glm::vec3 thirtySlope{ std::sin(glm::radians(30.0f)), cos30, 0.0f };
        EXPECT_TRUE(SlopeFilterPasses(thirtySlope, 0.71f));
    }

    TEST(ScatterBrushSlopeFilter, ZeroThresholdDisablesFilter)
    {
        // 80° slope — would fail any reasonable threshold, must pass when
        // the filter is explicitly disabled. Mirrors the "0 disables"
        // contract advertised in the panel tooltip.
        const f32 cos80 = std::cos(glm::radians(80.0f));
        const glm::vec3 steepSlope{ std::sin(glm::radians(80.0f)), cos80, 0.0f };
        EXPECT_TRUE(SlopeFilterPasses(steepSlope, 0.0f));
    }

    TEST(ScatterBrushSlopeFilter, OverhangFails)
    {
        // Downward normal (cave ceiling / overhang) — never a valid
        // foliage surface. Should fail any positive threshold.
        const glm::vec3 ceiling{ 0.0f, -1.0f, 0.0f };
        EXPECT_FALSE(SlopeFilterPasses(ceiling, 0.1f));
    }

    TEST(ScatterBrushVariantEncoding, SingleVariantProducesZero)
    {
        // N = 1: only one valid index (0), and Custom must be 0.0 (the
        // "no variants authored" default). Divide-by-zero would corrupt
        // the entire instance buffer's Custom column.
        EXPECT_EQ(VariantToCustom(0, 1), 0.0f);
    }

    TEST(ScatterBrushVariantEncoding, EndpointsAreExactlyZeroAndOne)
    {
        // For N > 1, variants 0 and N-1 must encode to 0.0 and 1.0
        // exactly — shader-side decode `i32(Custom * (N - 1))` relies on
        // this for boundary cases (the last variant should NOT round down
        // to N-2).
        EXPECT_EQ(VariantToCustom(0, 4), 0.0f);
        EXPECT_EQ(VariantToCustom(3, 4), 1.0f);
        EXPECT_EQ(VariantToCustom(0, 16), 0.0f);
        EXPECT_EQ(VariantToCustom(15, 16), 1.0f);
    }

    TEST(ScatterBrushVariantEncoding, OutOfRangeIndexClamps)
    {
        // The brush passes an unfiltered random index in [0, N) but the
        // helper must defend against the closed-interval edge case
        // (random_int<f32>(0, N) sometimes rolls exactly N before the
        // floor/cast). Without clamping the resulting Custom would be
        // > 1, which a shader-side `int(Custom * (N-1))` would alias to
        // a wrong variant.
        EXPECT_EQ(VariantToCustom(99, 4), 1.0f);
        EXPECT_EQ(VariantToCustom(-1, 4), 0.0f);
    }
} // namespace OloEngine::Tests
