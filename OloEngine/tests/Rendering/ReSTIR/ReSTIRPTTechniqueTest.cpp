// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTTechnique.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace OloEngine::Tests
{
    TEST(ReSTIRPTTechnique, SettingsRejectNonfiniteInputsAndClampWorkBudgets)
    {
        ReSTIRPTSettings settings;
        EXPECT_FALSE(settings.Enabled);
        settings.InitialCandidates = 0;
        settings.MappingMask = 0xFFFFFFFFu;
        settings.ConfidenceCap = std::numeric_limits<f32>::infinity();
        settings.NormalBias = std::numeric_limits<f32>::quiet_NaN();
        settings.DebugView = static_cast<ReSTIRPTDebugView>(99u);
        const auto sanitized = SanitizeReSTIRPTSettings(settings);
        EXPECT_EQ(sanitized.InitialCandidates, 1u);
        EXPECT_EQ(sanitized.MappingMask, 7u);
        EXPECT_FLOAT_EQ(sanitized.ConfidenceCap, 8.0f);
        EXPECT_FLOAT_EQ(sanitized.NormalBias, 0.01f);
        EXPECT_EQ(sanitized.DebugView, ReSTIRPTDebugView::Radiance);
        EXPECT_EQ(sanitized, SanitizeReSTIRPTSettings(sanitized));
    }

    TEST(ReSTIRPTTechnique, EveryEngagementCombinationHasExclusiveIndirectOwnership)
    {
        for (u32 bits = 0; bits < 512u; ++bits)
        {
            SCOPED_TRACE(bits);
            const auto bit = [bits](u32 i)
            { return (bits & (1u << i)) != 0u; };
            const ReSTIRPTOwnershipInputs inputs{
                bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6), bit(7), bit(8)
            };
            const auto ownership = SelectReSTIRPTOwnership(inputs);
            EXPECT_EQ(ownership.DIAtPrimary, inputs.DIActive);
            EXPECT_EQ(ownership.ConventionalDirectAtPrimary, !inputs.DIActive);
            EXPECT_EQ(ownership.PTIndirectDiffuse, inputs.PTActive);
            EXPECT_EQ(ownership.PTIndirectSpecular, inputs.PTActive);
            EXPECT_EQ(ownership.PTCacheTailReads, 0u);
            const auto& diffuse = ownership.DiffuseFallbacks;
            EXPECT_EQ(static_cast<u32>(ownership.PTIndirectDiffuse) +
                          static_cast<u32>(diffuse.ReSTIRGIAtPrimary) +
                          static_cast<u32>(diffuse.DDGIAtPrimary),
                      1u);
            if (inputs.PTActive)
            {
                EXPECT_FALSE(diffuse.DDGIAtSecondary);
                EXPECT_FALSE(diffuse.SSGIComposite);
                EXPECT_FALSE(ownership.SpecularIBL);
                EXPECT_FALSE(ownership.SSR);
                EXPECT_FALSE(ownership.RTReflection);
            }
            else
            {
                const auto expected = SelectIndirectDiffuseSources({
                    .ReSTIRGIActive = inputs.GIActive,
                    .SSGIRequested = inputs.SSGIActive,
                    .DDGITailRequested = inputs.GICacheTailRequested && inputs.DDGIAvailable,
                });
                EXPECT_EQ(diffuse, expected);
                EXPECT_EQ(ownership.SpecularIBL, inputs.SpecularIBLActive);
                EXPECT_EQ(ownership.SSR, inputs.SSRActive);
                EXPECT_EQ(ownership.RTReflection, inputs.RTReflectionActive);
            }
        }
    }

    TEST(ReSTIRPTTechnique, ConfidenceCountsScheduledSourcesOnceAndHonorsBothCaps)
    {
        const std::array sources{
            ReSTIRPTSourceConfidence{ 11u, 16.0f, 4.0f },
            ReSTIRPTSourceConfidence{ 12u, 3.0f, 8.0f },
            ReSTIRPTSourceConfidence{ 13u, 0.0f, 2.0f },
        };
        const auto combined = CombineReSTIRPTSourceConfidence(sources, 100.0f);
        ASSERT_TRUE(combined.Valid);
        EXPECT_FLOAT_EQ(combined.Confidence, 7.0f);
        EXPECT_FLOAT_EQ(CombineReSTIRPTSourceConfidence(sources, 5.0f).Confidence, 5.0f);
        const std::array reordered{ sources[2], sources[1], sources[0] };
        EXPECT_FLOAT_EQ(CombineReSTIRPTSourceConfidence(reordered, 100.0f).Confidence, 7.0f);
        const std::array duplicated{ sources[0], sources[0] };
        EXPECT_FALSE(CombineReSTIRPTSourceConfidence(duplicated, 100.0f).Valid);
        EXPECT_TRUE(CombineReSTIRPTSourceConfidence({}, 1.0f).Valid);
        EXPECT_FLOAT_EQ(CombineReSTIRPTSourceConfidence({}, 1.0f).Confidence, 0.0f);
    }

    TEST(ReSTIRPTTechnique, ConfidenceRejectsNonFiniteAndNegativeInputsEvenAfterSaturation)
    {
        const f32 largest = std::numeric_limits<f32>::max();
        const std::array huge{
            ReSTIRPTSourceConfidence{ 0u, largest, largest },
            ReSTIRPTSourceConfidence{ 1u, largest, largest },
        };
        const auto saturated = CombineReSTIRPTSourceConfidence(huge, largest);
        ASSERT_TRUE(saturated.Valid);
        EXPECT_FLOAT_EQ(saturated.Confidence, largest);
        const std::array invalid{
            -1.0f, std::numeric_limits<f32>::infinity(),
            -std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN()
        };
        for (const f32 value : invalid)
        {
            EXPECT_FALSE(CombineReSTIRPTSourceConfidence(huge, value).Valid);
            auto sources = huge;
            sources[1].Confidence = value;
            EXPECT_FALSE(CombineReSTIRPTSourceConfidence(sources, 1.0f).Valid);
            sources = huge;
            sources[1].ScheduledBudget = value;
            EXPECT_FALSE(CombineReSTIRPTSourceConfidence(sources, 1.0f).Valid);
        }
        EXPECT_FALSE(CombineReSTIRPTSourceConfidence(huge, 0.0f).Valid);
        auto duplicateAfterCap = huge;
        duplicateAfterCap[1].SourceSlot = duplicateAfterCap[0].SourceSlot;
        EXPECT_FALSE(CombineReSTIRPTSourceConfidence(duplicateAfterCap, 1.0f).Valid);
    }

    TEST(ReSTIRPTTechnique, TemporalInputMustBeThePreviousInitialReservoirWithOriginalLineage)
    {
        const ReSTIRPTHistoryRecord fresh{
            .LayoutVersion = 1u,
            .SceneEpoch = 42u,
            .WrittenFrame = 9u,
            .BirthFrame = 9u,
            .SourcePixel = 12u,
            .BirthPixel = 12u,
        };
        const auto validate = [](const auto& record)
        { return ValidateReSTIRPTHistory(record, 1u, 42u, 10u, true); };
        using Reason = ReSTIRPTHistoryRejection;
        EXPECT_EQ(validate(fresh), Reason::None);
        EXPECT_EQ(ValidateReSTIRPTHistory(fresh, 2u, 42u, 10u, true), Reason::Layout);
        EXPECT_EQ(ValidateReSTIRPTHistory(fresh, 0u, 42u, 10u, true), Reason::Layout);
        EXPECT_EQ(ValidateReSTIRPTHistory(fresh, 1u, 43u, 10u, true), Reason::Epoch);
        EXPECT_EQ(ValidateReSTIRPTHistory(fresh, 1u, 42u, 10u, false), Reason::Receiver);
        for (const u64 frame : std::array<u64, 4>{ 0u, 9u, 11u, std::numeric_limits<u64>::max() })
            EXPECT_EQ(ValidateReSTIRPTHistory(fresh, 1u, 42u, frame, true), Reason::Frame);
        for (const auto stage : { ReSTIRPTReservoirStage::Temporal, ReSTIRPTReservoirStage::Spatial,
                                  static_cast<ReSTIRPTReservoirStage>(99u) })
        {
            auto mixed = fresh;
            mixed.Stage = stage;
            EXPECT_EQ(validate(mixed), Reason::Stage);
        }
        auto mixed = fresh;
        mixed.ReuseDepth = 1u;
        EXPECT_EQ(validate(mixed), Reason::Lineage);
        mixed = fresh;
        mixed.BirthFrame = 8u;
        EXPECT_EQ(validate(mixed), Reason::Lineage);
        mixed = fresh;
        mixed.BirthPixel = 13u;
        EXPECT_EQ(validate(mixed), Reason::Lineage);
        mixed = fresh;
        mixed.WrittenFrame = std::numeric_limits<u64>::max();
        EXPECT_EQ(ValidateReSTIRPTHistory(mixed, 1u, 42u, 0u, true), Reason::Frame);
        for (const u64 frame : std::array<u64, 2>{ 1u, std::numeric_limits<u64>::max() })
        {
            auto boundary = fresh;
            boundary.WrittenFrame = frame - 1u;
            boundary.BirthFrame = frame - 1u;
            EXPECT_EQ(ValidateReSTIRPTHistory(boundary, 1u, 42u, frame, true), Reason::None);
        }
    }
} // namespace OloEngine::Tests
