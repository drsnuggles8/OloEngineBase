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

#include "OloEngine/Groom/CoatRebuildBudget.h"
#include "OloEngine/Groom/CoatVolumeAdmission.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomCoatShadowTechnique.h"

#include <cmath>
#include <limits>
#include <map>
#include <optional>
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

TEST(GroomCoatShadowSelection, ADeformingGroomBakedFromItsDrawnPoseIsDelivered)
{
    // #1426. Until then EVERY bound groom was refused here, which covered every
    // animal and every character. A deformed coat is now baked from the pose it
    // is drawn at, so a deformed groom with that pose in hand is simply a coat
    // that gets what it asked for.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;
    inputs.DeformedPoseAvailable = true;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::None);
    EXPECT_FALSE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::AnisotropicDensityVolume);
    EXPECT_EQ(decision.Slot, 0u);
}

TEST(GroomCoatShadowSelection, ADeformingGroomWithNoDrawnPoseIsRefusedRatherThanShadowedAtItsBindPose)
{
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;
    inputs.DeformedPoseAvailable = false;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    // With no drawn pose the only thing left to bake is the asset's REST
    // curves, and a coat carrying its bind-pose shadow around an animating body
    // reads as a shading bug. So it is refused, and COUNTED under its own name.
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::DeformedPoseUnavailable);
    EXPECT_TRUE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
    EXPECT_EQ(decision.Slot, kNoGroomCoatShadowSlot);
}

TEST(GroomCoatShadowSelection, TheDrawnPoseIsNeverAssumed)
{
    // Fail-closed default: a deformed groom must SUPPLY its pose. A default of
    // "available" would let a future producer that forgot to set the lane bake
    // whatever it had lying around -- the rest curves -- and report success.
    constexpr GroomCoatShadowInputs defaults{};
    static_assert(!defaults.DeformedPoseAvailable);

    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;
    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::DeformedPoseUnavailable);
}

TEST(GroomCoatShadowSelection, TheDrawnPoseLaneIsIgnoredForAnUndeformedGroom)
{
    // An unbound groom's bake reads the asset, so whether a drawn pose exists
    // is not its question. Refusing it here would take coat shadowing away
    // from every static groom that ever shipped.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = false;
    inputs.DeformedPoseAvailable = false;
    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::None);
}

TEST(GroomCoatShadowSelection, AStaleVolumeIsRefusedAndReadsUnshadowed)
{
    // A resident volume the coat has moved too far from. Sampling it would
    // shadow each strand by where its neighbours USED to be -- a shadow sliding
    // over a walking animal -- so it is refused and the coat reads fully lit
    // (rule 10) with a reason that says so.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;
    inputs.DeformedPoseAvailable = true;
    inputs.RepresentationStale = true;

    const GroomCoatShadowDecision decision = SelectGroomCoatShadow(inputs);
    EXPECT_EQ(decision.Reason, GroomCoatShadowFallbackReason::RepresentationStale);
    EXPECT_TRUE(decision.IsFallback());
    EXPECT_EQ(decision.Effective, GroomCoatShadowTechnique::None);
    EXPECT_EQ(decision.Slot, kNoGroomCoatShadowSlot);
}

TEST(GroomCoatShadowSelection, AnUnbuiltVolumeIsReportedAheadOfAStaleOne)
{
    // Ordering: "nothing is resident" is the more fundamental statement. A
    // stale flag on an entry with no volume describes a representation that
    // does not exist.
    GroomCoatShadowInputs inputs = FullyCapable();
    inputs.GroomIsDeformed = true;
    inputs.DeformedPoseAvailable = true;
    inputs.RepresentationReady = false;
    inputs.RepresentationStale = true;
    EXPECT_EQ(SelectGroomCoatShadow(inputs).Reason, GroomCoatShadowFallbackReason::RepresentationNotBuilt);
}

TEST(GroomCoatShadowSelection, ADeformingGroomThatNeverAskedIsStillNotAFallback)
{
    // Ordering: NotRequested is more fundamental than DeformedPoseUnavailable,
    // because a coat that never asked cannot have failed at anything. Most
    // bound grooms in a scene are in exactly this state, and counting them as
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

// The coat-volume budget (#1259): past the count, a newcomer is admitted only
// while the bytes fit, and a reclaim of one small volume does not admit a large
// one. Before AdmitCoatVolume the pass freed one least-recently-used volume and
// granted the slot whatever its size.
TEST(GroomCoatShadowSelection, ALargeNewcomerReclaimsUntilItFitsOrIsRefused)
{
    constexpr GroomCoatShadow::CoatVolumeBudget kBudget{ 8u, 256u * 1024u * 1024u };
    constexpr u64 kHerdVolume = 300u * 1024u;      // a 16^3 fox volume
    constexpr u64 kCloseUp = 200u * 1024u * 1024u; // a close-up coat

    // Forty herd volumes resident, none drawn this frame: the close-up fits
    // after reclaiming, and only as many as it needs.
    {
        u32 count = 40u;
        u64 bytes = 40u * kHerdVolume + 100u * 1024u * 1024u; // and one 100 MiB volume
        u32 reclaims = 0u;
        const bool admitted = GroomCoatShadow::AdmitCoatVolume(count, bytes, kCloseUp, kBudget, [&]() -> std::optional<u64>
                                                               {
                                                                   ++reclaims;
                                                                   return reclaims == 1u ? 100u * 1024u * 1024u : kHerdVolume; });
        EXPECT_TRUE(admitted);
        EXPECT_EQ(reclaims, 1u) << "freeing the 100 MiB volume is enough";
        EXPECT_EQ(count, 39u);
        EXPECT_LE(bytes + kCloseUp, kBudget.MaxBytes);
    }

    // Every resident volume is a small one: one reclaim does not make room,
    // and the close-up is refused once nothing more can be reclaimed rather
    // than granted past the budget.
    {
        u32 count = 20u;
        u64 bytes = 20u * 7u * 1024u * 1024u; // 140 MiB, still 20 volumes after two reclaims
        u32 reclaimable = 2u;
        const bool admitted = GroomCoatShadow::AdmitCoatVolume(count, bytes, kCloseUp, kBudget, [&]() -> std::optional<u64>
                                                               {
                                                                   if (reclaimable == 0u)
                                                                       return std::nullopt;
                                                                   --reclaimable;
                                                                   return 7u * 1024u * 1024u; });
        EXPECT_FALSE(admitted) << "granted a slot that leaves the set over its byte budget";
        EXPECT_EQ(reclaimable, 0u);
    }

    // Under the count, any size is admitted without reclaiming.
    {
        u32 count = 3u;
        u64 bytes = 0u;
        const bool admitted = GroomCoatShadow::AdmitCoatVolume(count, bytes, kCloseUp * 4u, kBudget, []() -> std::optional<u64>
                                                               {
                                                                   ADD_FAILURE() << "reclaimed under the count";
                                                                   return std::nullopt; });
        EXPECT_TRUE(admitted);
        EXPECT_EQ(count, 3u);
    }
}

// THE REBUILD BUDGET (#1594), acceptance: rebuilds per frame stay within the
// budget, and every deferral is counted. A camera cut moved ~44 coats across a
// coat-LOD step in one frame and every one rebuilt in it.
TEST(GroomCoatShadowSelection, RebuildsPerFrameStayWithinTheBudgetAndEveryDeferralIsCounted)
{
    GroomCoatShadow::CoatRebuildBudget budget;
    budget.SetPerFrame(2u);
    constexpr u32 kCoats = 44u;
    u32 deferredTotal = 0u;
    u32 frames = 0u;
    std::set<u64> done;
    for (u64 frame = 1u; frame <= 30u && done.size() < kCoats; ++frame)
    {
        ++frames;
        budget.BeginFrame(frame, frame);
        u32 granted = 0u;
        u32 deferred = 0u;
        for (u64 coat = 0u; coat < kCoats; ++coat)
        {
            if (done.contains(coat))
                continue;
            if (budget.Request(coat))
            {
                ++granted;
                done.insert(coat);
            }
            else
            {
                ++deferred;
            }
        }
        EXPECT_LE(granted, 2u) << "frame " << frame << " rebuilt past the budget";
        EXPECT_EQ(budget.GrantedThisFrame(), granted);
        EXPECT_EQ(budget.DeferredThisFrame(), deferred) << "a deferral went uncounted";
        deferredTotal += deferred;
    }
    EXPECT_EQ(done.size(), kCoats) << "a coat waiting for its rebuild was never served";
    EXPECT_EQ(frames, 22u) << "44 coats at 2 a frame";
    // 42 + 40 + ... + 2: every frame a coat waits is one deferral.
    EXPECT_EQ(deferredTotal, 462u);
    budget.BeginFrame(100u, 100u);
    EXPECT_EQ(budget.Waiting(), 0u) << "a served coat stayed in the queue";
}

// NO STARVATION (#1594): a coat that keeps being deferred is served, however
// many newcomers keep asking in front of it. Without the reservation, the coats
// iterated first every frame would take the budget forever.
TEST(GroomCoatShadowSelection, ADeferredCoatIsServedAheadOfNewcomers)
{
    GroomCoatShadow::CoatRebuildBudget budget;
    budget.SetPerFrame(2u);
    // Six coats wait from frame 1; each later frame, three NEW coats ask first.
    std::set<u64> waiting{ 100u, 101u, 102u, 103u, 104u, 105u };
    std::map<u64, u64> servedAt;
    u64 nextNewcomer = 1000u;
    for (u64 frame = 1u; frame <= 12u; ++frame)
    {
        budget.BeginFrame(frame, frame);
        if (frame > 1u)
        {
            for (u32 n = 0; n < 3u; ++n)
                (void)budget.Request(nextNewcomer++);
        }
        for (auto it = waiting.begin(); it != waiting.end();)
        {
            if (budget.Request(*it))
            {
                servedAt[*it] = frame;
                it = waiting.erase(it);
            }
            else
            {
                ++it;
            }
        }
        EXPECT_LE(budget.GrantedThisFrame(), 2u);
    }
    EXPECT_TRUE(waiting.empty()) << waiting.size() << " deferred coat(s) starved behind newcomers";
    // Two served on frame 1, then the oldest two, reserved, on each frame after.
    for (const auto& [coat, frame] : servedAt)
        EXPECT_LE(frame, 3u) << "coat " << coat << " waited past its turn";
    EXPECT_GT(budget.Waiting(), 0u) << "the newcomers should be the ones waiting now";
}

// A coat that stops asking (culled, or no longer needing its rebuild) leaves the
// queue at the next frame and stops holding a reservation; a second camera in
// the same frame spends the same budget; zero is no limit.
TEST(GroomCoatShadowSelection, TheRebuildQueueForgetsCoatsThatStopAskingAndSharesOneFramesBudget)
{
    GroomCoatShadow::CoatRebuildBudget budget;
    budget.SetPerFrame(1u);
    budget.BeginFrame(1u, 1u);
    EXPECT_TRUE(budget.Request(1u));
    EXPECT_FALSE(budget.Request(2u));
    EXPECT_FALSE(budget.Request(3u));
    EXPECT_EQ(budget.Waiting(), 2u);
    // The second camera of frame 1: the same frame id on the next pass, the
    // budget already spent.
    budget.BeginFrame(1u, 2u);
    EXPECT_FALSE(budget.Request(4u)) << "a repeat BeginFrame for the same frame refilled the budget";
    EXPECT_EQ(budget.DeferredThisFrame(), 3u);

    // Frame 2: coat 2 holds the reservation but is not drawn; coat 3 waits.
    budget.BeginFrame(2u, 2u);
    EXPECT_FALSE(budget.Request(3u)) << "coat 3 jumped coat 2's reservation";
    EXPECT_EQ(budget.OldestWaitFrames(), 1u);
    // Frame 3: coat 2 did not ask on frame 2, so it is gone, and coat 3 is first.
    budget.BeginFrame(3u, 3u);
    EXPECT_TRUE(budget.Request(3u));
    EXPECT_FALSE(budget.Request(4u));
    budget.BeginFrame(4u, 4u);
    EXPECT_TRUE(budget.Request(4u));
    budget.BeginFrame(5u, 5u);
    EXPECT_EQ(budget.Waiting(), 0u) << "a coat that stopped asking stayed queued";

    budget.SetPerFrame(0u);
    budget.BeginFrame(6u, 6u);
    for (u64 coat = 0u; coat < 100u; ++coat)
        EXPECT_TRUE(budget.Request(coat)) << "a zero budget limited a rebuild";
    EXPECT_EQ(budget.DeferredThisFrame(), 0u);
}

// A deferred pose bake keeps drawing its volume across a stride change (#1594),
// so its drift has to be measurable across one: the full bake against the
// subset pose and the subset bake against the full pose give the same answer as
// the same stride, and two different subsets are compared where they overlap.
TEST(GroomCoatShadowSelection, DriftIsMeasuredAcrossABakeSubsetStrideChange)
{
    using GroomCoatShadow::CoatSegment;
    std::vector<CoatSegment> full;
    for (u32 s = 0; s < 400u; ++s)
    {
        // Every segment moves by a different amount, so a misaligned pairing
        // reads a different drift than the aligned one.
        const glm::vec3 a(static_cast<f32>(s) * 0.01f, 0.0f, 0.0f);
        full.push_back(CoatSegment{ a, a + glm::vec3(0.0f, 0.01f, 0.0f), 0.001f, 0.001f });
    }
    std::vector<CoatSegment> moved = full;
    for (sizet s = 0; s < moved.size(); ++s)
    {
        const f32 shift = 0.001f * static_cast<f32>(s % 17u);
        moved[s].A.z += shift;
        moved[s].B.z += shift;
    }
    constexpr u32 kStride = 3u;
    std::vector<CoatSegment> subsetMoved;
    (void)GroomCoatShadow::SubsampleCoatSegments(moved, kStride, subsetMoved);
    std::vector<CoatSegment> subsetFull;
    (void)GroomCoatShadow::SubsampleCoatSegments(full, kStride, subsetFull);
    ASSERT_LT(subsetMoved.size(), moved.size());

    std::vector<glm::vec3> bakedFull;
    GroomCoatShadow::CaptureCoatPose(full, bakedFull);
    std::vector<glm::vec3> bakedSubset;
    GroomCoatShadow::CaptureCoatPose(subsetFull, bakedSubset);

    // The subset's own drift is what both cross-stride arms must report.
    const f32 subsetDrift = GroomCoatShadow::MaxCoatPoseDrift(bakedSubset, subsetMoved);
    ASSERT_GT(subsetDrift, 0.0f);
    EXPECT_FLOAT_EQ(GroomCoatShadow::MaxCoatPoseDriftAcrossStrides(bakedFull, 1u, subsetMoved, kStride), subsetDrift)
        << "a full bake against a subset pose";
    EXPECT_FLOAT_EQ(GroomCoatShadow::MaxCoatPoseDriftAcrossStrides(bakedSubset, kStride, moved, 1u), subsetDrift)
        << "a subset bake against the full pose";
    EXPECT_FLOAT_EQ(GroomCoatShadow::MaxCoatPoseDriftAcrossStrides(bakedFull, 1u, moved, 1u),
                    GroomCoatShadow::MaxCoatPoseDrift(bakedFull, moved));
    // Two different subsets are compared on the segments both keep -- an
    // infinite drift forced every pose-baked coat whose stride changed at a
    // camera cut past the rebuild budget. The expectation is computed here, on
    // the full poses, independently of the walk that pairs them.
    constexpr u32 kOtherStride = 5u;
    std::vector<CoatSegment> otherMoved;
    (void)GroomCoatShadow::SubsampleCoatSegments(moved, kOtherStride, otherMoved);
    f32 sharedDrift = 0.0f;
    u32 shared = 0;
    for (u32 s = 0; s < static_cast<u32>(full.size()); ++s)
    {
        if (GroomCoatShadow::CoatBakeKeepsSegment(s, kStride) && GroomCoatShadow::CoatBakeKeepsSegment(s, kOtherStride))
        {
            const glm::vec3 before = 0.5f * (full[s].A + full[s].B);
            const glm::vec3 after = 0.5f * (moved[s].A + moved[s].B);
            sharedDrift = std::max(sharedDrift, glm::length(after - before));
            ++shared;
        }
    }
    ASSERT_GT(shared, 0u);
    ASSERT_GT(sharedDrift, 0.0f);
    EXPECT_FLOAT_EQ(GroomCoatShadow::MaxCoatPoseDriftAcrossStrides(bakedSubset, kStride, otherMoved, kOtherStride), sharedDrift)
        << "a subset bake against another subset's pose";
    // And unmoved, the two subsets agree exactly where they overlap.
    std::vector<CoatSegment> otherFull;
    (void)GroomCoatShadow::SubsampleCoatSegments(full, kOtherStride, otherFull);
    EXPECT_FLOAT_EQ(GroomCoatShadow::MaxCoatPoseDriftAcrossStrides(bakedSubset, kStride, otherFull, kOtherStride), 0.0f);
}

// A GRANT THAT DOES NOT BAKE IS GIVEN BACK (#1594): a coat the resident-volume
// budget holds back after the rebuild budget granted it would otherwise spend
// one of the frame's few bakes on nothing and drop out of the queue. It rejoins
// behind the coats already waiting -- ahead of newcomers, but never holding the
// head's reservation while the resident budget keeps refusing it.
TEST(GroomCoatShadowSelection, AGrantThatCannotBakeIsRefundedAndStarvesNoWaiter)
{
    using GroomCoatShadow::CoatRebuildBudget;
    {
        CoatRebuildBudget budget;
        budget.SetPerFrame(1u);
        EXPECT_TRUE(budget.BeginFrame(1u, 1u)) << "the first pass of a frame starts it";
        EXPECT_FALSE(budget.BeginFrame(1u, 2u)) << "a second view's pass of the same frame does not";
        EXPECT_TRUE(budget.Request(1u));
        budget.Refund(1u, false);
        EXPECT_EQ(budget.GrantedThisFrame(), 0u) << "the grant went back to the frame";
        EXPECT_EQ(budget.Waiting(), 1u) << "the refunded coat left the queue";
        // Next frame it is served ahead of a newcomer.
        EXPECT_TRUE(budget.BeginFrame(2u, 3u));
        EXPECT_FALSE(budget.Request(9u)) << "a newcomer took the refunded coat's turn";
        EXPECT_TRUE(budget.Request(1u)) << "the refunded coat lost its place";
        // A refunded forced bake gives back its forced count too.
        budget.Force(7u);
        EXPECT_EQ(budget.ForcedThisFrame(), 1u);
        budget.Refund(7u, true);
        EXPECT_EQ(budget.ForcedThisFrame(), 0u);
    }
    // One bake a frame; coat 1 is at the head and the resident budget refuses
    // it every frame. Coat 2, waiting behind it, is still served -- in either
    // order of asking. Refunded to the front, coat 1 held the only reservation
    // for good and coat 2 starved.
    for (const bool headAsksFirst : { true, false })
    {
        CoatRebuildBudget budget;
        budget.SetPerFrame(1u);
        (void)budget.BeginFrame(1u, 1u);
        EXPECT_TRUE(budget.Request(1u));
        EXPECT_FALSE(budget.Request(2u));
        budget.Refund(1u, false);
        u64 servedAt = 0;
        for (u64 frame = 2u; frame <= 6u && servedAt == 0u; ++frame)
        {
            (void)budget.BeginFrame(frame, frame);
            const auto askHead = [&]()
            {
                if (budget.Request(1u))
                    budget.Refund(1u, false);
            };
            if (headAsksFirst)
                askHead();
            if (budget.Request(2u))
                servedAt = frame;
            if (!headAsksFirst)
                askHead();
        }
        EXPECT_EQ(servedAt, 2u) << "a waiter starved behind a coat the resident budget keeps refusing (head asks "
                                << (headAsksFirst ? "first" : "second") << ")";
    }
}

// A REBAKE DUE ON DRIFT IS FORCED (#1594): a moving coat's rebake always runs
// -- deferred, its shadow slides off the coat -- and it spends the budget, so on
// a frame of moving coats fewer other rebuilds run. Only forced bakes take a
// frame past the budget, and a reserved waiter is still served.
TEST(GroomCoatShadowSelection, ADriftRebakeIsForcedAndSpendsTheBudget)
{
    GroomCoatShadow::CoatRebuildBudget budget;
    budget.SetPerFrame(2u);
    budget.BeginFrame(1u, 1u);
    EXPECT_TRUE(budget.Request(1u));
    EXPECT_TRUE(budget.Request(2u));
    EXPECT_FALSE(budget.Request(3u)) << "coat 3 waits for frame 2";

    // Frame 2: three walking coats rebake on drift before coat 3 is reached.
    budget.BeginFrame(2u, 2u);
    for (u64 walker = 10u; walker < 13u; ++walker)
        budget.Force(walker);
    EXPECT_EQ(budget.ForcedThisFrame(), 3u);
    EXPECT_FALSE(budget.Request(4u)) << "a newcomer baked on a budget the forced bakes spent";
    EXPECT_TRUE(budget.Request(3u)) << "the reserved waiter must still be served";
    EXPECT_EQ(budget.GrantedThisFrame(), 4u) << "three forced and one reserved";
    EXPECT_EQ(budget.DeferredThisFrame(), 1u);

    // A forced bake of a waiting coat takes it out of the queue.
    budget.BeginFrame(3u, 3u);
    budget.Force(4u);
    budget.BeginFrame(4u, 4u);
    EXPECT_EQ(budget.Waiting(), 0u) << "a coat whose bake was forced stayed queued";
    EXPECT_EQ(budget.ForcedThisFrame(), 0u);
}

// The budget is the PHYSICAL frame's (#1594): every view's pass of one frame
// spends one budget, and a waiter one view draws keeps its place while another
// view renders. A host that never advances the frame id gets a new frame after
// kMaxPassesPerFrame passes rather than a budget that never refills. A coat that
// takes a shared bake while it waits leaves the queue and frees its reservation.
TEST(GroomCoatShadowSelection, TheRebuildBudgetIsTheFramesAcrossViewsAndAStalledFrameIdStillRefills)
{
    using GroomCoatShadow::CoatRebuildBudget;
    CoatRebuildBudget budget;
    budget.SetPerFrame(1u);
    // Frame 7, view A then view B.
    budget.BeginFrame(7u, 100u);
    EXPECT_TRUE(budget.Request(1u)) << "view A";
    budget.BeginFrame(7u, 101u);
    EXPECT_FALSE(budget.Request(2u)) << "view B of the same frame spent view A's budget again";
    EXPECT_EQ(budget.GrantedThisFrame(), 1u);
    // Frame 8: view A first, but coat 2 (view B's) holds the reservation and is
    // served when view B renders.
    budget.BeginFrame(8u, 102u);
    EXPECT_FALSE(budget.Request(3u)) << "a newcomer in view A took view B's reserved coat's turn";
    budget.BeginFrame(8u, 103u);
    EXPECT_TRUE(budget.Request(2u)) << "view B's waiting coat lost its place while view A rendered";

    // A host that leaves the frame id at 9: a new frame every kMaxPassesPerFrame passes.
    CoatRebuildBudget stalled;
    stalled.SetPerFrame(1u);
    stalled.BeginFrame(9u, 200u);
    EXPECT_TRUE(stalled.Request(10u));
    u32 refills = 0;
    for (u64 tick = 201u; tick < 200u + 3u * CoatRebuildBudget::kMaxPassesPerFrame; ++tick)
    {
        stalled.BeginFrame(9u, tick);
        if (stalled.GrantedThisFrame() == 0u && stalled.Request(11u + tick))
            ++refills;
    }
    EXPECT_EQ(refills, 2u) << "a stalled frame id must start a new frame every kMaxPassesPerFrame passes";

    // Withdraw: a reserved coat that took a shared bake frees its turn.
    CoatRebuildBudget shared;
    shared.SetPerFrame(1u);
    shared.BeginFrame(1u, 1u);
    EXPECT_TRUE(shared.Request(1u));
    EXPECT_FALSE(shared.Request(2u));
    shared.BeginFrame(2u, 2u);
    shared.Withdraw(2u);
    EXPECT_TRUE(shared.Request(3u)) << "a withdrawn reservation still blocked the frame";
    EXPECT_EQ(shared.Waiting(), 0u);
}

// A RESOLUTION CHANGE NEEDS NO FULL BAKE (#1594): the occupancy a coat measured
// at one resolution, scaled to another -- the cube for a finer grid (never fewer
// voxels than the coat has), the square for a coarser one (a coat's surface) --
// so for a shell or a pelt the stride never leaves fewer segments in a voxel
// than the floor asks. A sparser coat is the update rule's, below.
// At a camera cut the full re-measure of a walking coat was a 250 ms bake.
TEST(GroomCoatShadowSelection, AResolutionChangeEstimatesOccupancyOnTheSafeSide)
{
    using GroomCoatShadow::EstimateCoatOccupiedVoxels;
    EXPECT_EQ(EstimateCoatOccupiedVoxels(5000u, 64u, 64u), 5000u) << "the measured resolution is the measurement";
    EXPECT_EQ(EstimateCoatOccupiedVoxels(5000u, 64u, 128u), 40000u) << "a finer grid scales by the cube";
    EXPECT_EQ(EstimateCoatOccupiedVoxels(5000u, 64u, 32u), 1250u) << "a coarser grid scales by the square";
    EXPECT_EQ(EstimateCoatOccupiedVoxels(0u, 64u, 128u), 0u) << "nothing measured is nothing estimated";
    EXPECT_EQ(EstimateCoatOccupiedVoxels(5000u, 0u, 128u), 0u);
    EXPECT_GE(EstimateCoatOccupiedVoxels(1u, 128u, 8u), 1u) << "an occupied coat never estimates to empty";

    // On the safe side means the stride it gives never exceeds the one a full
    // bake would measure, for a coat whose occupancy grows with the grid by a
    // power from 2 (a thin shell) to 3 (a thick pelt), both ways.
    constexpr u64 kSegments = 2'000'000u;
    constexpr f32 kPerVoxel = 8.0f;
    for (const f64 exponent : { 2.0, 2.5, 3.0 })
    {
        for (const u32 to : { 16u, 32u, 128u, 256u })
        {
            const u32 measuredAt = 64u;
            const u32 measured = 6000u;
            const auto truth = static_cast<u32>(
                std::ceil(measured * std::pow(static_cast<f64>(to) / static_cast<f64>(measuredAt), exponent)));
            const u32 estimated = EstimateCoatOccupiedVoxels(measured, measuredAt, to);
            EXPECT_LE(GroomCoatShadow::CoatBakeSubsetStride(kSegments, estimated, kPerVoxel),
                      GroomCoatShadow::CoatBakeSubsetStride(kSegments, truth, kPerVoxel))
                << "exponent " << exponent << ", " << measuredAt << " -> " << to
                << ": the estimate took a coarser subset than a full bake would";
        }
    }
}

// WHAT A BAKE TEACHES THE OCCUPANCY (#1594): a full bake measures it; a subset
// bake that counted more than the estimate raises it (a subset only
// undercounts: a sparse coat on a coarser grid, which the square underestimates,
// is corrected after one bake); one that kept the floor's segments in every
// voxel it occupied counted the coat and replaces a high estimate, so a walking
// coat's rebakes after a cut are not each paid at the first bake's cautious
// stride. Anything else leaves the measurement alone.
TEST(GroomCoatShadowSelection, ABakeCorrectsTheOccupancyEstimateBothWays)
{
    using GroomCoatShadow::CoatOccupancy;
    using GroomCoatShadow::UpdateCoatOccupancy;
    constexpr f32 kPerVoxel = 8.0f;
    const CoatOccupancy measured{ 6000u, 64u };

    // Nothing measured yet: any bake is the measurement.
    const CoatOccupancy first = UpdateCoatOccupancy({}, 900u, 32u, 4u, 1000u, kPerVoxel);
    EXPECT_EQ(first.Voxels, 900u);
    EXPECT_EQ(first.Resolution, 32u);
    // A full bake always measures.
    const CoatOccupancy full = UpdateCoatOccupancy(measured, 1400u, 32u, 1u, 50'000u, kPerVoxel);
    EXPECT_EQ(full.Voxels, 1400u);
    EXPECT_EQ(full.Resolution, 32u);
    // A sparse coat on a coarser grid: the square estimates 1500, the subset
    // already counted 3000 -- the estimate was low, so it is raised.
    const CoatOccupancy sparse = UpdateCoatOccupancy(measured, 3000u, 32u, 16u, 6000u, kPerVoxel);
    EXPECT_EQ(sparse.Voxels, 3000u) << "an undercounting estimate was not raised";
    // A pelt on a coarser grid: the square estimates 1500, a bake that kept 20
    // segments in each of the 600 voxels it occupied counted the coat -- the
    // estimate was high, so it is replaced.
    const CoatOccupancy dense = UpdateCoatOccupancy(measured, 600u, 32u, 3u, 12'000u, kPerVoxel);
    EXPECT_EQ(dense.Voxels, 600u) << "a densely sampled bake did not correct a high estimate";
    EXPECT_EQ(dense.Resolution, 32u);
    // A thinly sampled subset that counted fewer than the estimate proves
    // nothing: the measurement stands.
    const CoatOccupancy thin = UpdateCoatOccupancy(measured, 600u, 32u, 40u, 1200u, kPerVoxel);
    EXPECT_EQ(thin.Voxels, measured.Voxels);
    EXPECT_EQ(thin.Resolution, measured.Resolution);
    // At the measurement's own grid a dense subset only undercounts it: it must
    // not replace it, or the occupancy shrinks, the stride grows and every
    // frame rebakes a different subset of a coat that is not moving.
    CoatOccupancy settled{ 6000u, 64u };
    for (u32 bake = 0; bake < 8u; ++bake)
    {
        settled = UpdateCoatOccupancy(settled, 5200u, 64u, 3u, 60'000u, kPerVoxel);
    }
    EXPECT_EQ(settled.Voxels, 6000u) << "repeated subset bakes at the measured grid shrank the measurement";
    // A bake that occupied nothing teaches nothing.
    const CoatOccupancy empty = UpdateCoatOccupancy(measured, 0u, 32u, 1u, 0u, kPerVoxel);
    EXPECT_EQ(empty.Voxels, measured.Voxels);
}
