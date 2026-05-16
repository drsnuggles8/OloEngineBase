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

// NOTE(olo): Disabled — calling PrecipitationSystem static methods forces the
// linker to pull in PrecipitationSystem.obj and its transitive GPU/Mono
// dependencies, which crash during static initialisation (no OpenGL context
// available in the test harness).  Re-enable once a headless test fixture or
// mock is in place.
#if 0
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
#endif

// =============================================================================
// Retired: LerpIntensityConverges, LerpIntensityMovesSmoothly,
// FrameBudgetReducesEmission, FrameBudgetRecoversWhenUnderBudget.
//
// All four reimplemented the algorithm inline in the test body and then
// asserted on the local copy -- pure liability per
// docs/testing.md section 4.2 (inline-reimplemented algorithm).
// They cannot fail when the real PrecipitationSystem changes because
// they never call into it. Re-enable once a headless PrecipitationController
// type exists that can be invoked without a GL context (coverage gap section 7).
// =============================================================================

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
