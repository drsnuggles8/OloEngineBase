#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCoatShadowSelectionTest — issue #1248, acceptance criterion 3.
//
// "Representation resolution and update policy are inspectable."
//
// SelectGroomCoatShadow is the CPU half of that: it is the only place that
// decides what a coat actually gets, and the only place that can say why. So
// every reason is covered here with ONE input knocked out at a time, in the
// shape ShadowTechniqueSelectionTest established and GroomCompositionSelection
// followed — a case that disabled two things at once would pass while
// reporting whichever reason the implementation happened to check first, which
// is exactly the drift these cases exist to catch.
//
// The fallback arm is also the arm CI runs: there is no GPU here, so the
// machine running this file is the machine that would take every fallback.
// That is the argument for putting the decision on the CPU at all.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomCoatShadowTechnique.h"

#include <set>
#include <string>

using namespace OloEngine;

namespace
{
    // A frame that can do everything, so each case below removes exactly one
    // capability and names the reason that removal causes.
    [[nodiscard]] GroomCoatShadowInputs FullyCapable()
    {
        GroomCoatShadowInputs inputs;
        inputs.Requested = GroomCoatShadowTechnique::AnisotropicDensityVolume;
        inputs.SegmentCount = 140000u;
        inputs.VolumeTexturesSupported = true;
        inputs.HasDirectionalLight = true;
        inputs.ResolvedResolution = 64u;
        inputs.MinResolution = 8u;
        inputs.RepresentationReady = true;
        inputs.GrantedSlot = 0u;
        return inputs;
    }
} // namespace

// ── The delivered case ───────────────────────────────────────────────────────

TEST(GroomCoatShadowSelection, AFullyCapableFrameDeliversWhatWasAsked)
{
    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(FullyCapable());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::AnisotropicDensityVolume);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::None);
    EXPECT_EQ(decision.Slot, 0u);
    EXPECT_FALSE(decision.IsFallback());
}

TEST(GroomCoatShadowSelection, TheDecisionIsConstexpr)
{
    // Not decoration: a decision that can be taken at compile time is a
    // decision that reaches no renderer state, which is the property that
    // makes it testable on a machine with no GPU.
    constexpr GroomCoatShadowInputs inputs{};
    static_assert(SelectGroomCoatShadow(inputs).Reason == GroomCoatShadowFallbackReason::NotRequested);
    static_assert(SelectGroomCoatShadow(inputs).Effective == GroomCoatShadowTechnique::None);
    SUCCEED();
}

// ── One reason at a time ─────────────────────────────────────────────────────

TEST(GroomCoatShadowSelection, ACoatThatNeverAskedIsNotAFallback)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.Requested = GroomCoatShadowTechnique::None;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::NotRequested);
    // THE POINT OF THIS CASE: a scene full of grooms that simply do not use
    // the feature must not report every one of them as a failure, or the
    // counter that explains a missing shadow is saturated by coats working
    // exactly as authored.
    EXPECT_FALSE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
}

TEST(GroomCoatShadowSelection, AnEmptyGroomReportsItsOwnGeometryRatherThanTheCache)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.SegmentCount = 0u;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::GroomHasNoGeometry);
    EXPECT_TRUE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
}

TEST(GroomCoatShadowSelection, ADeformingGroomIsRefusedRatherThanShadowedAtItsBindPose)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    // The bake reads the asset's REST-POSE curves, so on an animating body the
    // drawn strands move and the volume does not. A coat carrying its bind-pose
    // shadow around reads as a shading bug rather than as the missing feature
    // it is, so it is refused and COUNTED.
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::GroomIsDeformed);
    EXPECT_TRUE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
    EXPECT_EQ(decision.Slot, kNoGroomCoatShadowSlot);
}

TEST(GroomCoatShadowSelection, ADeformingGroomThatNeverAskedIsStillNotAFallback)
{
    // Ordering: NotRequested is more fundamental than GroomIsDeformed, because
    // a coat that never asked cannot have failed at anything. Most bound
    // grooms in a scene are in exactly this state, and counting them as
    // failures would bury the ones that really did ask.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.Requested = GroomCoatShadowTechnique::None;
    inputs.GroomIsDeformed = true;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::NotRequested);
    EXPECT_FALSE(decision.IsFallback());
}

TEST(GroomCoatShadowSelection, ADeviceWithNo3DTextureRefusesBothVolumeModes)
{
    for (const GroomCoatShadowTechnique mode : { GroomCoatShadowTechnique::IsotropicDensityVolume,
                                                 GroomCoatShadowTechnique::AnisotropicDensityVolume })
    {
        GroomCoatShadowInputs inputs = FullyCapable();
        inputs.Requested = mode;
        inputs.VolumeTexturesSupported = false;

        const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
        EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::VolumeTexturesUnavailable)
            << "mode " << GroomCoatShadow::ToString(mode);
    }
}

TEST(GroomCoatShadowSelection, TheRejectedDeepOpacityMapIsRefusedRatherThanSilentlySubstituted)
{
    // THE CASE THAT CAUGHT A REAL SILENT FALLBACK. An earlier version of this
    // file asserted the opposite — that a deep-map request was DELIVERED — and
    // it passed, because the seam said yes. Downstream, the pass does not
    // branch on the mode when it bakes: it built a DENSITY VOLUME, handed the
    // shader mode 3, and the shader's only test (`mode != ANISOTROPIC`)
    // marched that volume isotropically. So the coat rendered, the counters
    // reported the deep map as delivered, and the picture was a different
    // representation than the one asked for.
    //
    // A rejected candidate that still reports success is worse than one that
    // is missing, because nothing looks wrong.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.Requested = GroomCoatShadowTechnique::DeepOpacityMap;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::ModeNotImplemented);
    EXPECT_TRUE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
    EXPECT_EQ(decision.Slot, kNoGroomCoatShadowSlot);
}

TEST(GroomCoatShadowSelection, TheImplementedSetIsExactlyWhatAShaderRenders)
{
    // The bake-off measures four modes and the renderer implements three.
    // Pinning the difference here is what stops a future mode being added to
    // the comparison and silently becoming selectable.
    EXPECT_TRUE(GroomCoatShadowModeIsImplemented(GroomCoatShadowTechnique::None));
    EXPECT_TRUE(GroomCoatShadowModeIsImplemented(GroomCoatShadowTechnique::IsotropicDensityVolume));
    EXPECT_TRUE(GroomCoatShadowModeIsImplemented(GroomCoatShadowTechnique::AnisotropicDensityVolume));
    EXPECT_FALSE(GroomCoatShadowModeIsImplemented(GroomCoatShadowTechnique::DeepOpacityMap));
}

TEST(GroomCoatShadowSelection, AnUnimplementedModeIsRefusedBeforeAnyCapabilityCheck)
{
    // Ordering: "nothing renders this" is a permanent property of the request,
    // like a hardware limit, so it must win over a per-frame fact. Reporting
    // VolumeTexturesUnavailable for a mode that does not use a 3D texture
    // would be a reason that is not true.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.Requested = GroomCoatShadowTechnique::DeepOpacityMap;
    inputs.VolumeTexturesSupported = false;
    inputs.HasDirectionalLight = false;
    inputs.GroomIsDeformed = true;

    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::ModeNotImplemented);
}

TEST(GroomCoatShadowSelection, ThePerLightGuardIsReachableAndNamesItsOwnReason)
{
    // The NoDirectionalLight arm is currently UNREACHABLE through a real
    // request, because the only per-light mode is the rejected deep map and
    // that is refused first. It is covered here directly rather than deleted:
    // the guard is what a future per-light representation would need, and an
    // uncovered reason is one nobody would notice had stopped working.
    //
    // Asserted against the predicate rather than a hand-built decision, so this
    // case starts exercising the real path the moment such a mode is wired.
    ASSERT_TRUE(GroomCoatShadow::CoatShadowModeIsPerLight(GroomCoatShadowTechnique::DeepOpacityMap));
    ASSERT_FALSE(GroomCoatShadowModeIsImplemented(GroomCoatShadowTechnique::DeepOpacityMap))
        << "a per-light mode is implemented now — point this case at it and drop the note above";

    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.Requested = GroomCoatShadowTechnique::DeepOpacityMap;
    inputs.HasDirectionalLight = false;
    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::ModeNotImplemented);
}

TEST(GroomCoatShadowSelection, ALightIndependentModeDoesNotNeedADirectionalLight)
{
    // A density volume is built from the coat alone. Making it depend on a
    // light would be the same wrong-reason failure as the 3D-texture case
    // above, and it is the property the whole selection rests on — see
    // CoatShadowModeIsPerLight.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.HasDirectionalLight = false;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::None);
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::AnisotropicDensityVolume);
}

TEST(GroomCoatShadowSelection, AResolutionBelowTheFloorFallsBackRatherThanMarchingNoise)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.ResolvedResolution = 4u;
    inputs.MinResolution = 8u;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::ResolutionBelowFloor);
}

TEST(GroomCoatShadowSelection, AnExhaustedBudgetIsItsOwnReason)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GrantedSlot = kNoGroomCoatShadowSlot;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::BudgetExhausted);
    EXPECT_EQ(decision.Slot, kNoGroomCoatShadowSlot);
}

TEST(GroomCoatShadowSelection, AnUnbuiltRepresentationIsNormalAndDistinctFromAHardwareLimit)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.RepresentationReady = false;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    // Deliberately NOT VolumeTexturesUnavailable. One is normal on a coat's
    // first visible frame; the other is a hardware fact for the whole session.
    // Conflating them turns a warm-up into a hardware diagnosis.
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::RepresentationNotBuilt);
}

// ── Ordering ─────────────────────────────────────────────────────────────────

TEST(GroomCoatShadowSelection, TheMostFundamentalReasonWinsWhenSeveralApply)
{
    // A groom with no geometry would also have nothing built and no slot. The
    // reported reason must be the one the user can act on, not the last
    // symptom in the chain.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.SegmentCount = 0u;
    inputs.GroomIsDeformed = true;
    inputs.RepresentationReady = false;
    inputs.GrantedSlot = kNoGroomCoatShadowSlot;
    inputs.ResolvedResolution = 2u;

    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::GroomHasNoGeometry);
}

TEST(GroomCoatShadowSelection, ARefusedCoatNeverCarriesAResourceSlot)
{
    // A slot handed to a coat that is not going to sample it is a slot the
    // budget thinks is spent, and the shader would read whatever is in it.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.SegmentCount = 0u;

    EXPECT_EQ(SelectGroomCoatShadow(inputs).Slot, kNoGroomCoatShadowSlot);
}

// ── The reasons themselves ───────────────────────────────────────────────────

TEST(GroomCoatShadowSelection, EveryReasonHasADistinctNonEmptySentence)
{
    // A counter a user cannot act on is a counter nobody reads, and a
    // ToString returning "unknown" is the shape of that failure.
    std::set<std::string> sentences;
    std::set<std::string> tokens;
    for (u32 i = 0; i < static_cast<u32>(GroomCoatShadowFallbackReason::Count); ++i)
    {
        const auto reason = static_cast<GroomCoatShadowFallbackReason>(i);
        const std::string sentence{ Describe(reason) };
        const std::string token{ ToString(reason) };

        EXPECT_FALSE(sentence.empty()) << "reason " << i;
        EXPECT_NE(sentence, "Unknown") << "reason " << i;
        EXPECT_NE(token, "Unknown") << "reason " << i;
        // A sentence, not a token: it must end like one.
        EXPECT_EQ(sentence.back(), '.') << "reason " << i << " is not a sentence: " << sentence;
        sentences.insert(sentence);
        tokens.insert(token);
    }
    EXPECT_EQ(sentences.size(), static_cast<sizet>(GroomCoatShadowFallbackReason::Count));
    EXPECT_EQ(tokens.size(), static_cast<sizet>(GroomCoatShadowFallbackReason::Count));
}

TEST(GroomCoatShadowSelection, EveryModeHasADistinctNonEmptyName)
{
    std::set<std::string> names;
    for (u32 i = 0; i < static_cast<u32>(GroomCoatShadow::CoatShadowMode::Count); ++i)
    {
        const auto mode = static_cast<GroomCoatShadow::CoatShadowMode>(i);
        const std::string name{ GroomCoatShadow::ToString(mode) };
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "Unknown") << "mode " << i;
        names.insert(name);
    }
    EXPECT_EQ(names.size(), static_cast<sizet>(GroomCoatShadow::CoatShadowMode::Count));
}

TEST(GroomCoatShadowSelection, ModeValidationRejectsTheEnumeratorCount)
{
    // Count is the enumerator count, not a mode. Accepting it would put a mode
    // nobody authored into the renderer — the same reason GroomComponent's
    // composition mode is Reject rather than Clamp.
    EXPECT_TRUE(GroomCoatShadow::IsValidCoatShadowMode(0));
    EXPECT_TRUE(GroomCoatShadow::IsValidCoatShadowMode(
        static_cast<i32>(GroomCoatShadow::CoatShadowMode::Count) - 1));
    EXPECT_FALSE(GroomCoatShadow::IsValidCoatShadowMode(static_cast<i32>(GroomCoatShadow::CoatShadowMode::Count)));
    EXPECT_FALSE(GroomCoatShadow::IsValidCoatShadowMode(-1));
    EXPECT_FALSE(GroomCoatShadow::IsValidCoatShadowMode(7));
}

TEST(GroomCoatShadowSelection, OnlyTheDeepOpacityMapIsPerLight)
{
    // The whole selection rests on this: a light-independent representation
    // cannot go stale when the light moves, which is acceptance criterion 4
    // answered structurally rather than by tuning.
    EXPECT_FALSE(GroomCoatShadow::CoatShadowModeIsPerLight(GroomCoatShadow::CoatShadowMode::None));
    EXPECT_FALSE(
        GroomCoatShadow::CoatShadowModeIsPerLight(GroomCoatShadow::CoatShadowMode::IsotropicDensityVolume));
    EXPECT_FALSE(
        GroomCoatShadow::CoatShadowModeIsPerLight(GroomCoatShadow::CoatShadowMode::AnisotropicDensityVolume));
    EXPECT_TRUE(GroomCoatShadow::CoatShadowModeIsPerLight(GroomCoatShadow::CoatShadowMode::DeepOpacityMap));
}

// ── Stats ────────────────────────────────────────────────────────────────────

TEST(GroomCoatShadowStatsTest, DeliveredRefusedAndNeverAskedAreCountedApart)
{
    GroomCoatShadowStats stats;

    stats.Record(SelectGroomCoatShadow(FullyCapable()));

    GroomCoatShadowInputs unasked = FullyCapable();
    unasked.Requested = GroomCoatShadowTechnique::None;
    stats.Record(SelectGroomCoatShadow(unasked));
    stats.Record(SelectGroomCoatShadow(unasked));

    GroomCoatShadowInputs refused = FullyCapable();
    refused.GrantedSlot = kNoGroomCoatShadowSlot;
    stats.Record(SelectGroomCoatShadow(refused));

    EXPECT_EQ(stats.ShadowedGrooms, 1u);
    EXPECT_EQ(stats.UnshadowedByChoice, 2u);
    EXPECT_EQ(stats.FallbackGrooms, 1u);
    EXPECT_EQ(stats.ByReason[static_cast<sizet>(GroomCoatShadowFallbackReason::BudgetExhausted)], 1u);
}

TEST(GroomCoatShadowStatsTest, TheDominantReasonIgnoresCoatsThatNeverAsked)
{
    GroomCoatShadowStats stats;
    GroomCoatShadowInputs unasked = FullyCapable();
    unasked.Requested = GroomCoatShadowTechnique::None;
    for (int i = 0; i < 100; ++i)
    {
        stats.Record(SelectGroomCoatShadow(unasked));
    }
    // A hundred coats not using the feature must not drown the one that failed.
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCoatShadowFallbackReason::None);

    GroomCoatShadowInputs refused = FullyCapable();
    refused.RepresentationReady = false;
    stats.Record(SelectGroomCoatShadow(refused));
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCoatShadowFallbackReason::RepresentationNotBuilt);
}

TEST(GroomCoatShadowStatsTest, TheDominantReasonIsTheMostFundamentalOnePresent)
{
    GroomCoatShadowStats stats;

    GroomCoatShadowInputs late = FullyCapable();
    late.RepresentationReady = false;
    for (int i = 0; i < 50; ++i)
    {
        stats.Record(SelectGroomCoatShadow(late));
    }

    GroomCoatShadowInputs empty = FullyCapable();
    empty.SegmentCount = 0u;
    stats.Record(SelectGroomCoatShadow(empty));

    // Fifty warm-ups against one broken import: the import is what a user can
    // fix, so it is what gets reported.
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCoatShadowFallbackReason::GroomHasNoGeometry);
}

TEST(GroomCoatShadowStatsTest, ResetClearsEveryLane)
{
    GroomCoatShadowStats stats;
    stats.Record(SelectGroomCoatShadow(FullyCapable()));
    stats.ResolutionInForce = 64u;
    stats.Rebuilds = 3u;
    stats.ResidentBytes = 1234u;
    stats.Reset();
    EXPECT_EQ(stats, GroomCoatShadowStats{});
}
