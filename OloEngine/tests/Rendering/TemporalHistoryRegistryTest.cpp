// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/TemporalHistoryRegistry.h"

#include <gtest/gtest.h>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr auto kAllViewDependencies = TemporalHistoryDependency::ViewTransform |
                                              TemporalHistoryDependency::Projection |
                                              TemporalHistoryDependency::Viewport |
                                              TemporalHistoryDependency::RenderScale |
                                              TemporalHistoryDependency::Scene |
                                              TemporalHistoryDependency::Backend |
                                              TemporalHistoryDependency::FeatureState |
                                              TemporalHistoryDependency::Jitter;

        TemporalHistoryDescriptor MakeDescriptor(u32 width = 640, u32 height = 360)
        {
            return {
                .Width = width,
                .Height = height,
                .Format = ImageFormat::RGBA16F,
                .Backend = TemporalHistoryBackend::OpenGL,
            };
        }

        TemporalHistoryKey MakeKey(TemporalHistoryEffect effect = TemporalHistoryEffect::SSGI,
                                   TemporalHistoryPlane plane = TemporalHistoryPlane::Signal)
        {
            return {
                .Effect = effect,
                .View = 42,
                .Resolution = TemporalHistoryResolution::Half,
                .Plane = plane,
            };
        }
    } // namespace

    TEST(TemporalHistoryRegistry, CompatibleAcquireKeepsTheSameGeneration)
    {
        TemporalHistoryRegistry registry;
        const auto first = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGI.Signal");
        const auto second = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGI.Signal");

        EXPECT_TRUE(first.Created);
        EXPECT_FALSE(second.Created);
        EXPECT_FALSE(second.DescriptorChanged);
        EXPECT_EQ(first.Token, second.Token);
    }

    TEST(TemporalHistoryRegistry, DuplicateDebugNameForDifferentTypedKeyIsRejected)
    {
        TemporalHistoryRegistry registry;
        const auto first = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGI.Signal");
        const auto collision = registry.Acquire(
            MakeKey(TemporalHistoryEffect::SSGI, TemporalHistoryPlane::MomentsFirst),
            MakeDescriptor(), kAllViewDependencies, "SSGI.Signal");

        EXPECT_TRUE(first.Token.IsValid());
        EXPECT_FALSE(collision.Token.IsValid());
        EXPECT_EQ(registry.Snapshot().size(), 1u);
    }

    TEST(TemporalHistoryRegistry, DescriptorMismatchAdvancesGenerationAndRejectsStaleToken)
    {
        TemporalHistoryRegistry registry;
        const auto first = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies);
        const auto resized = registry.Acquire(MakeKey(), MakeDescriptor(800, 450), kAllViewDependencies);

        EXPECT_TRUE(resized.DescriptorChanged);
        EXPECT_EQ(resized.Token.Generation, first.Token.Generation + 1);
        EXPECT_FALSE(registry.IsCurrent(first.Token));
        EXPECT_FALSE(registry.IsValid(resized.Token));
    }

    TEST(TemporalHistoryRegistry, ShaderLayoutMismatchAdvancesGeneration)
    {
        TemporalHistoryRegistry registry;
        const auto first = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies);
        auto changedLayout = MakeDescriptor();
        changedLayout.LayoutVersion = 2u;
        const auto changed = registry.Acquire(MakeKey(), changedLayout, kAllViewDependencies);

        EXPECT_TRUE(changed.DescriptorChanged);
        EXPECT_EQ(changed.Token.Generation, first.Token.Generation + 1u);
        EXPECT_FALSE(registry.IsCurrent(first.Token));
    }

    TEST(TemporalHistoryRegistry, GenerationRolloverNeverProducesTheInvalidSentinel)
    {
        EXPECT_EQ(NextTemporalHistoryGeneration(std::numeric_limits<u32>::max()), 1u);
    }

    TEST(TemporalHistoryRegistry, CameraCutInvalidatesOnlyViewDependentHistories)
    {
        TemporalHistoryRegistry registry;
        const auto viewHistory = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies);
        const auto persistentHistory = registry.Acquire(
            MakeKey(TemporalHistoryEffect::Cloudscape), MakeDescriptor(),
            TemporalHistoryDependency::Scene | TemporalHistoryDependency::Backend);

        EXPECT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::CameraCut), 1u);
        EXPECT_FALSE(registry.IsCurrent(viewHistory.Token));
        EXPECT_TRUE(registry.IsCurrent(persistentHistory.Token));
    }

    TEST(TemporalHistoryRegistry, FeatureToggleInvalidatesOnlyTheSelectedEffect)
    {
        TemporalHistoryRegistry registry;
        const auto ssgi = registry.Acquire(MakeKey(TemporalHistoryEffect::SSGI), MakeDescriptor(), kAllViewDependencies);
        const auto ssr = registry.Acquire(MakeKey(TemporalHistoryEffect::SSR), MakeDescriptor(), kAllViewDependencies);

        EXPECT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::FeatureToggled,
                                      TemporalHistoryEffect::SSGI),
                  1u);
        EXPECT_FALSE(registry.IsCurrent(ssgi.Token));
        EXPECT_TRUE(registry.IsCurrent(ssr.Token));
    }

    TEST(TemporalHistoryRegistry, BackendSwitchInvalidatesEveryBackendDependentPlane)
    {
        TemporalHistoryRegistry registry;
        const auto signal = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies);
        const auto moments = registry.Acquire(MakeKey(TemporalHistoryEffect::SSGI, TemporalHistoryPlane::MomentsFirst),
                                              MakeDescriptor(), kAllViewDependencies);

        EXPECT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::BackendChanged), 2u);
        EXPECT_FALSE(registry.IsCurrent(signal.Token));
        EXPECT_FALSE(registry.IsCurrent(moments.Token));
    }

    TEST(TemporalHistoryRegistry, EveryLifecycleCauseTargetsItsDeclaredDependency)
    {
        constexpr std::array cases{
            std::pair{ TemporalHistoryInvalidationCause::ProjectionChanged, TemporalHistoryDependency::Projection },
            std::pair{ TemporalHistoryInvalidationCause::ViewportResized, TemporalHistoryDependency::Viewport },
            std::pair{ TemporalHistoryInvalidationCause::DynamicResolutionChanged, TemporalHistoryDependency::RenderScale },
            std::pair{ TemporalHistoryInvalidationCause::SceneReset, TemporalHistoryDependency::Scene },
            std::pair{ TemporalHistoryInvalidationCause::JitterReset, TemporalHistoryDependency::Jitter },
        };

        for (const auto& [cause, dependency] : cases)
        {
            TemporalHistoryRegistry registry;
            const auto dependent = registry.Acquire(MakeKey(), MakeDescriptor(), dependency);
            const auto unrelated = registry.Acquire(
                MakeKey(TemporalHistoryEffect::Cloudscape), MakeDescriptor(), TemporalHistoryDependency::Backend);

            EXPECT_EQ(registry.Invalidate(cause), 1u);
            EXPECT_FALSE(registry.IsCurrent(dependent.Token));
            EXPECT_TRUE(registry.IsCurrent(unrelated.Token));
        }
    }

    TEST(TemporalHistoryRegistry, SnapshotReportsDescriptorGenerationAndLastInvalidation)
    {
        TemporalHistoryRegistry registry;
        const auto acquired = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGI.Signal");
        ASSERT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::SceneReset), 1u);

        const auto snapshots = registry.Snapshot();
        ASSERT_EQ(snapshots.size(), 1u);
        EXPECT_EQ(snapshots[0].Key, MakeKey());
        EXPECT_EQ(snapshots[0].Descriptor, MakeDescriptor());
        EXPECT_EQ(snapshots[0].Token.Generation, acquired.Token.Generation + 1u);
        EXPECT_EQ(snapshots[0].LastInvalidation, TemporalHistoryInvalidationCause::SceneReset);
        EXPECT_EQ(snapshots[0].DebugName, "SSGI.Signal");
        EXPECT_FALSE(snapshots[0].Valid);
    }
} // namespace OloEngine::Tests
