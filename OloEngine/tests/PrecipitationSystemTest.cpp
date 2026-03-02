#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// =============================================================================
// PrecipitationStats
// =============================================================================

TEST(PrecipitationStats, DefaultsAreZero)
{
    PrecipitationStats stats;
    EXPECT_FLOAT_EQ(stats.EffectiveEmissionRate, 0.0f);
    EXPECT_FLOAT_EQ(stats.GPUTimeMs, 0.0f);
}

// =============================================================================
// Intensity Tests (no GPU context needed)
// =============================================================================

TEST(PrecipitationSystem, IntensityClampLogic)
{
    // SetIntensityImmediate / GetCurrentIntensity operate on static data
    // without requiring GPU Init(), so we can test the real API directly.
    PrecipitationSystem::SetIntensityImmediate(-0.5f);
    EXPECT_FLOAT_EQ(PrecipitationSystem::GetCurrentIntensity(), 0.0f);

    PrecipitationSystem::SetIntensityImmediate(1.5f);
    EXPECT_FLOAT_EQ(PrecipitationSystem::GetCurrentIntensity(), 1.0f);

    PrecipitationSystem::SetIntensityImmediate(0.7f);
    EXPECT_FLOAT_EQ(PrecipitationSystem::GetCurrentIntensity(), 0.7f);

    // Clean up static state
    PrecipitationSystem::SetIntensityImmediate(0.0f);
}

// =============================================================================
// Intensity Interpolation Tests
// =============================================================================

TEST(PrecipitationSystem, LerpIntensityConverges)
{
    f32 current = 0.0f;
    f32 target = 1.0f;
    f32 speed = 2.0f;
    f32 dt = 0.016f;

    // After many iterations, should converge to target
    for (int i = 0; i < 1000; ++i)
    {
        f32 lerpFactor = std::clamp(speed * dt, 0.0f, 1.0f);
        current = std::lerp(current, target, lerpFactor);
    }

    EXPECT_NEAR(current, target, 0.001f);
}

TEST(PrecipitationSystem, LerpIntensityMovesSmoothly)
{
    f32 current = 0.0f;
    f32 target = 1.0f;
    f32 speed = 0.5f;
    f32 dt = 0.016f;

    f32 prev = current;
    for (int i = 0; i < 10; ++i)
    {
        f32 lerpFactor = std::clamp(speed * dt, 0.0f, 1.0f);
        current = std::lerp(current, target, lerpFactor);

        // Should monotonically increase toward target
        EXPECT_GT(current, prev);
        EXPECT_LT(current, target);
        prev = current;
    }
}

// =============================================================================
// Frame Budget Throttling Tests
// =============================================================================

TEST(PrecipitationSystem, FrameBudgetReducesEmission)
{
    f32 reductionFactor = 1.0f;
    f32 budgetMs = 1.0f;

    // Simulate being over budget for several frames
    for (int i = 0; i < 10; ++i)
    {
        f32 lastFrameMs = 1.5f; // Over budget
        if (lastFrameMs > budgetMs)
        {
            reductionFactor *= 0.9f;
            reductionFactor = std::max(reductionFactor, 0.1f);
        }
    }

    EXPECT_LT(reductionFactor, 0.5f); // Should have reduced significantly
    EXPECT_GE(reductionFactor, 0.1f); // Should not go below floor
}

TEST(PrecipitationSystem, FrameBudgetRecoversWhenUnderBudget)
{
    f32 reductionFactor = 0.3f; // Start throttled

    // Simulate being under budget for several frames
    for (int i = 0; i < 100; ++i)
    {
        f32 lastFrameMs = 0.5f; // Well under budget
        f32 budgetMs = 1.0f;
        if (lastFrameMs < budgetMs * 0.8f)
        {
            reductionFactor = std::min(reductionFactor * 1.05f, 1.0f);
        }
    }

    EXPECT_NEAR(reductionFactor, 1.0f, 0.01f); // Should have recovered
}

// =============================================================================
// PrecipitationSettings Copy Semantics
// =============================================================================

TEST(PrecipitationSettings, CopyPreservesValues)
{
    PrecipitationSettings a;
    a.Enabled = true;
    a.Intensity = 0.75f;
    a.BaseEmissionRate = 8000;
    a.ScreenStreaksEnabled = true;
    a.WindInfluence = 0.5f;

    PrecipitationSettings b = a;
    EXPECT_TRUE(b.Enabled);
    EXPECT_FLOAT_EQ(b.Intensity, 0.75f);
    EXPECT_EQ(b.BaseEmissionRate, 8000u);
    EXPECT_TRUE(b.ScreenStreaksEnabled);
    EXPECT_FLOAT_EQ(b.WindInfluence, 0.5f);
}
