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
        // SceneContent is deliberately NOT part of the view mask: it is what an
        // accumulating history (the GPU path tracer) declares because it cannot
        // reproject, and the reprojecting SSGI/SSR fixtures here must not.
        // Its cause mapping is pinned in EveryLifecycleCauseTargetsItsDeclaredDependency.

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

    TEST(TemporalHistoryRegistry, ArrayGrowthPreservesHistoryNamesAndTokens)
    {
        TemporalHistoryRegistry registry;
        std::array<TemporalHistoryToken, 128> tokens;
        for (u32 i = 0; i < tokens.size(); ++i)
        {
            auto key = MakeKey();
            key.View = i;
            const std::string name = std::string(i % 2 == 0 ? 3 : 128, 'x') + std::to_string(i);
            const auto acquired = registry.Acquire(key, MakeDescriptor(), kAllViewDependencies, name);
            ASSERT_TRUE(acquired.Created);
            tokens[i] = acquired.Token;
        }

        const auto snapshots = registry.Snapshot();
        ASSERT_EQ(snapshots.Num(), tokens.size());
        for (u32 i = 0; i < tokens.size(); ++i)
        {
            const std::string name = std::string(i % 2 == 0 ? 3 : 128, 'x') + std::to_string(i);
            EXPECT_TRUE(registry.IsCurrent(tokens[i]));
            EXPECT_EQ(registry.GetDebugName(tokens[i]), name);
            EXPECT_EQ(snapshots[i].DebugName.ToView(), name);
            EXPECT_EQ(snapshots[i].Token, tokens[i]);
        }
    }

    // The registry's contribution to the render-graph declaration key (issue
    // #1333) is WHICH histories exist and which are valid, never the
    // generation. Invalidate() advances the generation of every matching entry,
    // already-invalid ones included, so a key that hashed it would rebuild the
    // frame graph on every frame an object moved, once any scene-dependent
    // history had ever been armed.
    TEST(TemporalHistoryRegistry, ValidityKeyIgnoresGenerationsButSeesNewHistories)
    {
        TemporalHistoryRegistry registry;
        const u64 empty = registry.ComputeValidityKey();

        const auto acquired = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGIHistory");
        ASSERT_TRUE(acquired.Created);
        const u64 one = registry.ComputeValidityKey();
        EXPECT_NE(one, empty) << "A new history is a new import candidate: it must move the key.";

        const u32 invalidated = registry.Invalidate(TemporalHistoryInvalidationCause::Manual);
        ASSERT_GT(invalidated, 0u);
        EXPECT_FALSE(registry.IsCurrent(acquired.Token)) << "The generation advanced; the old token is stale.";
        EXPECT_EQ(registry.ComputeValidityKey(), one)
            << "An invalid history invalidated again declares exactly what it did before.";

        const auto second =
            registry.Acquire(MakeKey(TemporalHistoryEffect::SSR), MakeDescriptor(), kAllViewDependencies, "SSRHistory");
        ASSERT_TRUE(second.Created);
        EXPECT_NE(registry.ComputeValidityKey(), one);
    }

    // A holder that latched a token when a history was acquired (the render
    // graph's history sinks) follows later invalidations through Current(),
    // because an invalidation bumps the generation without replacing anything.
    TEST(TemporalHistoryRegistry, CurrentFollowsInvalidationsOfTheSameSlot)
    {
        TemporalHistoryRegistry registry;
        const auto acquired = registry.Acquire(MakeKey(), MakeDescriptor(), kAllViewDependencies, "SSGIHistory");
        ASSERT_TRUE(acquired.Created);
        EXPECT_EQ(registry.Current(acquired.Token), acquired.Token);

        (void)registry.Invalidate(TemporalHistoryInvalidationCause::Manual);
        (void)registry.Invalidate(TemporalHistoryInvalidationCause::Manual);
        const TemporalHistoryToken current = registry.Current(acquired.Token);
        EXPECT_EQ(current.Index, acquired.Token.Index);
        EXPECT_NE(current.Generation, acquired.Token.Generation);
        EXPECT_TRUE(registry.IsCurrent(current));
        EXPECT_FALSE(registry.IsCurrent(acquired.Token));

        EXPECT_FALSE(registry.Current(TemporalHistoryToken{}).IsValid()) << "An invalid token has no current slot.";
        EXPECT_FALSE(registry.Current(TemporalHistoryToken{ 999u, 1u }).IsValid()) << "Nor does one past the end.";
    }

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
        EXPECT_EQ(registry.Snapshot().Num(), 1u);
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
            std::pair{ TemporalHistoryInvalidationCause::SceneMutated, TemporalHistoryDependency::SceneContent },
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
        ASSERT_EQ(snapshots.Num(), 1u);
        EXPECT_EQ(snapshots[0].Key, MakeKey());
        EXPECT_EQ(snapshots[0].Descriptor, MakeDescriptor());
        EXPECT_EQ(snapshots[0].Token.Generation, acquired.Token.Generation + 1u);
        EXPECT_EQ(snapshots[0].LastInvalidation, TemporalHistoryInvalidationCause::SceneReset);
        EXPECT_EQ(snapshots[0].DebugName, "SSGI.Signal");
        EXPECT_FALSE(snapshots[0].Valid);
    }
} // namespace OloEngine::Tests
