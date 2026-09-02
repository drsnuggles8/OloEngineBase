// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/SurfaceHistory.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    namespace
    {
        SurfaceHistoryRecord MakeMatchingSurface()
        {
            SurfaceHistoryRecord surface{};
            surface.LinearDepth = 10.0f;
            surface.GeometricNormal = { 0.0f, 1.0f, 0.0f };
            surface.ShadingNormal = surface.GeometricNormal;
            surface.Roughness = 0.4f;
            surface.MaterialClass = 2u;
            surface.Instance = { 7u, 3u };
            surface.Primitive = { 11u, 2u };
            surface.Material = { 5u, 4u };
            return surface;
        }
    } // namespace

    TEST(SurfaceHistoryValidity, DifferentStableInstanceAtTheSameDepthIsRejected)
    {
        SurfaceHistoryRecord current = MakeMatchingSurface();
        SurfaceHistoryRecord previous = current;
        previous.Instance = { 8u, 3u };

        const SurfaceHistoryValidity result = EvaluateSurfaceHistory(
            current, previous, { 0.5f, 0.5f }, SurfaceHistoryValiditySettings{});

        EXPECT_FALSE(result.Accepted());
        EXPECT_TRUE(result.Has(SurfaceHistoryRejection::InstanceMismatch));
        EXPECT_FALSE(result.Has(SurfaceHistoryRejection::DepthMismatch));
    }

    TEST(SurfaceHistoryValidity, MaterialGenerationMismatchAtSimilarDepthIsRejected)
    {
        const SurfaceHistoryRecord current = MakeMatchingSurface();
        SurfaceHistoryRecord previous = current;
        previous.Material.m_Generation++;

        const auto result = EvaluateSurfaceHistory(
            current, previous, { 0.5f, 0.5f }, SurfaceHistoryValiditySettings{});

        EXPECT_TRUE(result.Has(SurfaceHistoryRejection::MaterialMismatch));
        EXPECT_FALSE(result.Has(SurfaceHistoryRejection::DepthMismatch));
    }

    TEST(SurfaceHistoryValidity, MissingCanonicalIdentityRejectsInsteadOfAliasingZero)
    {
        SurfaceHistoryRecord current = MakeMatchingSurface();
        SurfaceHistoryRecord previous = current;
        current.Instance = {};
        previous.Instance = {};

        const auto result = EvaluateSurfaceHistory(
            current, previous, { 0.5f, 0.5f }, SurfaceHistoryValiditySettings{});

        EXPECT_TRUE(result.Has(SurfaceHistoryRejection::IdentityUnavailable));
    }

    TEST(SurfaceHistoryValidity, DisabledIdentityChannelsDoNotRequireUnavailableHandles)
    {
        SurfaceHistoryRecord current = MakeMatchingSurface();
        SurfaceHistoryRecord previous = current;
        current.Primitive = {};
        previous.Primitive = {};
        current.Material = {};
        previous.Material = {};

        SurfaceHistoryValiditySettings settings{};
        settings.TestPrimitive = false;
        settings.TestMaterial = false;
        const auto result = EvaluateSurfaceHistory(current, previous, { 0.5f, 0.5f }, settings);
        EXPECT_TRUE(result.Accepted());
    }

    TEST(SurfaceHistoryValidity, DisabledSurfaceChannelsDoNotRejectUnavailablePayloads)
    {
        SurfaceHistoryRecord current = MakeMatchingSurface();
        SurfaceHistoryRecord previous = current;
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        current.GeometricNormal = previous.GeometricNormal = glm::vec3(nan);
        current.ShadingNormal = previous.ShadingNormal = glm::vec3(nan);
        current.Roughness = previous.Roughness = nan;
        current.Motion = glm::vec2(nan);
        current.HitDistance = previous.HitDistance = nan;

        SurfaceHistoryValiditySettings settings{};
        settings.TestGeometricNormal = false;
        settings.TestShadingNormal = false;
        settings.TestRoughness = false;
        settings.TestMotion = false;
        settings.TestHitDistance = false;
        const auto result = EvaluateSurfaceHistory(current, previous, { 0.5f, 0.5f }, settings);
        EXPECT_TRUE(result.Accepted());
    }

    TEST(SurfaceHistoryValidity, FirstFrameMomentsReturnCurrentHalfResolutionSignal)
    {
        TemporalMoments stale{};
        stale.First = glm::vec4(0.0f);
        stale.Second = glm::vec4(0.0f);
        stale.HistoryLength = 128.0f;
        const glm::vec4 current(0.8f, 0.4f, 0.2f, 1.0f);

        const TemporalMoments accumulated = AccumulateTemporalMoments(current, stale, false);

        EXPECT_TRUE(Math::BitwiseEqual(accumulated.First, current));
        EXPECT_TRUE(Math::BitwiseEqual(accumulated.Second, current * current));
        EXPECT_FLOAT_EQ(accumulated.HistoryLength, 1.0f);
    }

    TEST(SurfaceHistoryValidity, ScalarVisibilityUsesTheSameMomentsContract)
    {
        TemporalMoments previous{};
        previous.First.x = 0.25f;
        previous.Second.x = 0.0625f;
        previous.HistoryLength = 1.0f;

        const auto accumulated = AccumulateTemporalMoments(glm::vec4(0.75f, 0.0f, 0.0f, 0.0f), previous, true);

        EXPECT_FLOAT_EQ(accumulated.First.x, 0.5f);
        EXPECT_FLOAT_EQ(accumulated.Second.x, 0.3125f);
        EXPECT_FLOAT_EQ(accumulated.HistoryLength, 2.0f);
        EXPECT_GE(TemporalVariance(accumulated).x, 0.0f);
    }

    TEST(SurfaceHistoryValidity, RejectedHistoryCannotContaminateMomentsWithNan)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        TemporalMoments poisoned{
            .First = glm::vec4(nan),
            .Second = glm::vec4(nan),
            .HistoryLength = nan,
        };
        const glm::vec4 current(0.8f, 0.4f, 0.2f, 1.0f);

        const TemporalMoments accumulated = AccumulateTemporalMoments(current, poisoned, false);

        EXPECT_TRUE(Math::BitwiseEqual(accumulated.First, current));
        EXPECT_TRUE(Math::BitwiseEqual(accumulated.Second, current * current));
        EXPECT_FLOAT_EQ(accumulated.HistoryLength, 1.0f);
    }
} // namespace OloEngine::Tests
