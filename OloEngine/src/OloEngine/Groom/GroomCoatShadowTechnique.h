#pragma once

// =============================================================================
// GroomCoatShadowTechnique.h — which representation answers "how much coat is
// between this strand and the light", and why it is not the one that was asked
// for. Issue #1248.
//
// THE SELECTION IS A VALUE, NOT A BRANCH. This is the shape
// docs/agent-rules/technique-selection-seams.md sets out and that #1056's
// ShadowTechnique.h and #1246's GroomVisibility.h already follow here. A coat
// shadow can fail to be delivered for at least eight unrelated reasons — the
// groom cooked to nothing, the volume has not been built yet, the LOD asked for
// a resolution below the floor, the device has no 3D texture, the budget is
// spent — and written as a chain of ifs inside the pass, the interesting
// question ("this coat asked for a deep opacity map and did not get one, why?")
// becomes unanswerable from anywhere. The shader cannot say, because by then the
// reason is a uniform; the CPU cannot say either, because nothing on the CPU
// made the decision.
//
// So: a pure function from gathered facts to an effective mode, a reason, and
// the resource slot it was granted. It is constexpr and touches no renderer
// state, so it compiles and is tested on a machine with no GPU — which is every
// CI runner this project has, and which is also the machine that takes the
// FALLBACK path. The arm CI covers best is therefore the arm that most needs
// covering, and that is deliberate.
//
// WHY THE EFFECTIVE TECHNIQUE IS GroomCoatShadow::CoatShadowMode AND NOT A NEW
// ENUM. The bake-off in GroomCoatShadow.h measures modes; the renderer ships
// one. If those were two enums the comparison would be measuring something the
// runtime merely resembles, and a mode could be renamed on one side only. They
// are the same enumerator, so a row in the analysis document and a branch in
// the pass cannot drift apart.
//
// WHY NOT A FIELD ON ShadowSettings. Coat shadowing is a property of a GROOM,
// not of the scene's lighting: two coats in one frame legitimately want
// different resolutions and different update policies, and a scene-level knob
// could not express that. ShadowSettings owns what the LIGHT does;
// GroomCoatShadowComponent owns what a coat does. The same split #1246 made
// between RendererSettings and GroomComponent, for the same reason.
//
// THE ROUTING IS WRITTEN BY THE SUCCESS PATH. GroomStrandParamsUBO's coat lanes
// go up INACTIVE with the rest of the per-draw data, and only the code that has
// a built, bound representation in hand turns them on. Every way the build can
// fail therefore leaves the strand shader on the unshadowed branch by
// construction, rather than by someone remembering to reset a flag on each of
// six early returns. An unshadowed coat is #1247's picture, which is a
// legitimate state; a coat shadowed by a representation that was not built is
// not.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoatShadow.h"

#include <array>
#include <string_view>

namespace OloEngine
{
    // The representation a coat gets. Same enumerator the comparison measures.
    using GroomCoatShadowTechnique = GroomCoatShadow::CoatShadowMode;

    // No resident representation was granted. A NAMED sentinel, never a bare
    // -1: the index rides a UBO lane that the shader compares against, and a
    // magic number in two places is how one of them gets it wrong.
    inline constexpr u32 kNoGroomCoatShadowSlot = 0xFFFFFFFFu;

    // -------------------------------------------------------------------------
    // Why it is not what was asked for
    // -------------------------------------------------------------------------

    // ORDERED MOST-FUNDAMENTAL FIRST, and the FIRST match is the one reported.
    // A coat that trips an earlier guard would trip the later ones too, so
    // reporting the first gives a dominant reason the user can act on rather
    // than the last symptom in the chain.
    enum class GroomCoatShadowFallbackReason : u32
    {
        /// Delivered what was asked for.
        None = 0,

        /// The coat never asked for shadowing. NOT a failure and deliberately
        /// NOT counted as one: a scene full of grooms that simply do not use
        /// the feature would otherwise report every one of them as a fallback,
        /// and the counter that is supposed to explain a missing shadow would
        /// be saturated by coats that are working exactly as authored.
        NotRequested,

        /// The groom has no curves, or every curve was rejected. There is
        /// nothing to build a representation OF, so this is the coat's own
        /// import being empty rather than anything about shadowing.
        GroomHasNoGeometry,

        /// The groom is bound to a deforming surface. The bake reads the
        /// asset's REST-POSE curves, so on an animating body the drawn strands
        /// move and the volume does not — the coat would carry its bind-pose
        /// shadow around, which reads as a shading bug rather than as the
        /// missing feature it is. Refused and counted instead of approximated.
        GroomIsDeformed,

        /// This device cannot sample a 3D texture, so neither volume mode can
        /// run. Distinct from RepresentationNotBuilt on purpose: one is a
        /// hardware fact that will never change within the session, the other
        /// is normal on frame one, and conflating them turns a warm-up into a
        /// hardware diagnosis.
        VolumeTexturesUnavailable,

        /// A per-light representation was asked for and the frame resolved no
        /// directional light to build it for. A deep opacity map is defined by
        /// a light direction; with no light there is no map, and falling back
        /// to a volume is better than building one for an arbitrary direction.
        NoDirectionalLight,

        /// The shadow LOD policy asked for a resolution below its own floor —
        /// the coat is too small on screen for the representation to resolve
        /// anything. Falling back is the honest answer; marching a 4^3 grid
        /// would cost real time to produce noise.
        ResolutionBelowFloor,

        /// The resident-representation budget is spent. Every slot is held by
        /// a coat that was reached first this frame.
        BudgetExhausted,

        /// Nothing has been built yet. NORMAL on the first frame a coat is
        /// visible, and on the frame after its geometry changed — which is why
        /// it is last and why it must not be confused with the ones above.
        RepresentationNotBuilt,

        Count
    };

    // A SENTENCE per reason, not a token. A counter a user cannot act on is a
    // counter nobody reads, and "unknown" is the shape of that failure — so
    // every enumerator gets a real one and a test asserts none is empty.
    [[nodiscard]] constexpr std::string_view Describe(GroomCoatShadowFallbackReason reason)
    {
        switch (reason)
        {
            case GroomCoatShadowFallbackReason::None:
                return "The coat got the representation it asked for.";
            case GroomCoatShadowFallbackReason::NotRequested:
                return "This groom did not ask for coat shadowing, so it renders unshadowed by choice.";
            case GroomCoatShadowFallbackReason::GroomHasNoGeometry:
                return "The groom has no curves to build a shadow representation from.";
            case GroomCoatShadowFallbackReason::GroomIsDeformed:
                return "This groom is bound to a deforming surface, and the coat volume does not follow a "
                       "deformation yet.";
            case GroomCoatShadowFallbackReason::VolumeTexturesUnavailable:
                return "This device cannot sample a 3D texture, so no density volume can be built.";
            case GroomCoatShadowFallbackReason::NoDirectionalLight:
                return "A deep opacity map needs a directional light and the frame resolved none.";
            case GroomCoatShadowFallbackReason::ResolutionBelowFloor:
                return "The coat is too small on screen for the shadow LOD to resolve it above the resolution floor.";
            case GroomCoatShadowFallbackReason::BudgetExhausted:
                return "Every resident coat-shadow slot is already held by another groom this frame.";
            case GroomCoatShadowFallbackReason::RepresentationNotBuilt:
                return "The representation has not been built yet; this is normal on a coat's first visible frame.";
            case GroomCoatShadowFallbackReason::Count:
                break;
        }
        return "Unknown";
    }

    [[nodiscard]] constexpr std::string_view ToString(GroomCoatShadowFallbackReason reason)
    {
        switch (reason)
        {
            case GroomCoatShadowFallbackReason::None:
                return "None";
            case GroomCoatShadowFallbackReason::NotRequested:
                return "NotRequested";
            case GroomCoatShadowFallbackReason::GroomHasNoGeometry:
                return "GroomHasNoGeometry";
            case GroomCoatShadowFallbackReason::GroomIsDeformed:
                return "GroomIsDeformed";
            case GroomCoatShadowFallbackReason::VolumeTexturesUnavailable:
                return "VolumeTexturesUnavailable";
            case GroomCoatShadowFallbackReason::NoDirectionalLight:
                return "NoDirectionalLight";
            case GroomCoatShadowFallbackReason::ResolutionBelowFloor:
                return "ResolutionBelowFloor";
            case GroomCoatShadowFallbackReason::BudgetExhausted:
                return "BudgetExhausted";
            case GroomCoatShadowFallbackReason::RepresentationNotBuilt:
                return "RepresentationNotBuilt";
            case GroomCoatShadowFallbackReason::Count:
                break;
        }
        return "Unknown";
    }

    // -------------------------------------------------------------------------
    // What the frame resolved
    // -------------------------------------------------------------------------

    // EVERY FIELD IS A RESULT, never a request. The one exception is
    // `Requested`, which is named so. That distinction is the whole reason this
    // is testable: a decision made from what the frame EXPECTS to resolve is a
    // decision made from a guess, and #1246's GroomCompositionInputs carries the
    // same warning for the same reason.
    struct GroomCoatShadowInputs
    {
        /// What the component asked for.
        GroomCoatShadowTechnique Requested = GroomCoatShadowTechnique::None;

        /// Segments the groom actually produced. Zero means there is nothing to
        /// shadow with.
        u32 SegmentCount = 0;

        /// This groom's strands are deformed by a body's pose this frame.
        /// A RESULT, like everything else here: it is what the frame resolved,
        /// not what the component asked for.
        bool GroomIsDeformed = false;

        /// The device can create and sample a 3D texture.
        bool VolumeTexturesSupported = true;

        /// The frame resolved a directional light to build a per-light
        /// representation against.
        bool HasDirectionalLight = true;

        /// The resolution the shadow LOD policy settled on for this coat, after
        /// hysteresis.
        u32 ResolvedResolution = 64;

        /// The policy's own floor. Below it the representation resolves
        /// nothing.
        u32 MinResolution = 8;

        /// A built, uploaded representation is available for this coat THIS
        /// frame. Not "a build was requested" — the frame's own answer.
        bool RepresentationReady = false;

        /// The resident slot this coat holds, or kNoGroomCoatShadowSlot.
        u32 GrantedSlot = kNoGroomCoatShadowSlot;

        [[nodiscard]] auto operator==(const GroomCoatShadowInputs&) const -> bool = default;
    };

    struct GroomCoatShadowDecision
    {
        GroomCoatShadowTechnique Effective = GroomCoatShadowTechnique::None;
        GroomCoatShadowFallbackReason Reason = GroomCoatShadowFallbackReason::None;
        u32 Slot = kNoGroomCoatShadowSlot;

        /// True when the coat asked for something it did not get. A coat that
        /// never asked is NOT a fallback — see NotRequested.
        [[nodiscard]] constexpr bool IsFallback() const noexcept
        {
            return Reason != GroomCoatShadowFallbackReason::None &&
                   Reason != GroomCoatShadowFallbackReason::NotRequested;
        }

        [[nodiscard]] auto operator==(const GroomCoatShadowDecision&) const -> bool = default;
    };

    // The decision. Pure, constexpr, no renderer state reached from inside.
    //
    // The guards run most-fundamental first and the first match wins, so the
    // reason names the thing the user can fix. A coat with no geometry would
    // also have no built representation and no granted slot; reporting
    // "RepresentationNotBuilt" for it would send someone looking at a cache.
    [[nodiscard]] constexpr GroomCoatShadowDecision SelectGroomCoatShadow(const GroomCoatShadowInputs& inputs) noexcept
    {
        GroomCoatShadowDecision decision;

        if (inputs.Requested == GroomCoatShadowTechnique::None)
        {
            decision.Effective = GroomCoatShadowTechnique::None;
            decision.Reason = GroomCoatShadowFallbackReason::NotRequested;
            return decision;
        }

        if (inputs.SegmentCount == 0)
        {
            decision.Reason = GroomCoatShadowFallbackReason::GroomHasNoGeometry;
            return decision;
        }

        if (inputs.GroomIsDeformed)
        {
            decision.Reason = GroomCoatShadowFallbackReason::GroomIsDeformed;
            return decision;
        }

        const bool wantsVolume = inputs.Requested == GroomCoatShadowTechnique::IsotropicDensityVolume ||
                                 inputs.Requested == GroomCoatShadowTechnique::AnisotropicDensityVolume;
        if (wantsVolume && !inputs.VolumeTexturesSupported)
        {
            decision.Reason = GroomCoatShadowFallbackReason::VolumeTexturesUnavailable;
            return decision;
        }

        if (GroomCoatShadow::CoatShadowModeIsPerLight(inputs.Requested) && !inputs.HasDirectionalLight)
        {
            decision.Reason = GroomCoatShadowFallbackReason::NoDirectionalLight;
            return decision;
        }

        if (inputs.ResolvedResolution < inputs.MinResolution)
        {
            decision.Reason = GroomCoatShadowFallbackReason::ResolutionBelowFloor;
            return decision;
        }

        if (inputs.GrantedSlot == kNoGroomCoatShadowSlot)
        {
            decision.Reason = GroomCoatShadowFallbackReason::BudgetExhausted;
            return decision;
        }

        if (!inputs.RepresentationReady)
        {
            decision.Reason = GroomCoatShadowFallbackReason::RepresentationNotBuilt;
            return decision;
        }

        decision.Effective = inputs.Requested;
        decision.Reason = GroomCoatShadowFallbackReason::None;
        decision.Slot = inputs.GrantedSlot;
        return decision;
    }

    // -------------------------------------------------------------------------
    // What actually happened, for the panel and the PR's evidence
    // -------------------------------------------------------------------------

    struct GroomCoatShadowStats
    {
        /// Grooms that got the representation they asked for.
        u32 ShadowedGrooms = 0;
        /// Grooms that asked and did not get it. EXCLUDES the ones that never
        /// asked, which is what keeps this number meaning "something went
        /// wrong" rather than "most scenes do not use this".
        u32 FallbackGrooms = 0;
        /// Grooms that did not ask.
        u32 UnshadowedByChoice = 0;

        std::array<u32, static_cast<sizet>(GroomCoatShadowFallbackReason::Count)> ByReason{};

        /// The resolution actually in force, and the LOD step it came from.
        /// Acceptance criterion 3 asks for the representation's resolution and
        /// update policy to be INSPECTABLE; these two are that, and they are
        /// counters rather than a log line so the panel can show them without
        /// the renderer having to have said anything.
        u32 ResolutionInForce = 0;
        u32 LodStepInForce = 0;
        /// Representation rebuilds this frame. Steady-state should be ZERO for
        /// a static coat and one per moved coat; anything else is the update
        /// policy thrashing, which is the "stale density / flicker" criterion's
        /// failure mode showing up as a number before it shows up as a picture.
        u32 Rebuilds = 0;
        /// Frames since each resident representation was last rebuilt, maximum
        /// over the resident set. A number that climbs without bound on a
        /// MOVING coat is staleness.
        u32 MaxAgeFrames = 0;
        /// GPU bytes resident across every coat representation.
        u64 ResidentBytes = 0;

        void Record(const GroomCoatShadowDecision& decision) noexcept
        {
            ByReason[static_cast<sizet>(decision.Reason)] += 1u;
            if (decision.Reason == GroomCoatShadowFallbackReason::NotRequested)
            {
                ++UnshadowedByChoice;
            }
            else if (decision.IsFallback())
            {
                ++FallbackGrooms;
            }
            else
            {
                ++ShadowedGrooms;
            }
        }

        // The reason to put in front of a user. First match in enum order, so
        // it names the most fundamental thing that went wrong rather than the
        // most common symptom. NotRequested is skipped: it is not a failure.
        [[nodiscard]] GroomCoatShadowFallbackReason DominantFallbackReason() const noexcept
        {
            for (sizet i = 0; i < ByReason.size(); ++i)
            {
                const auto reason = static_cast<GroomCoatShadowFallbackReason>(i);
                if (reason == GroomCoatShadowFallbackReason::None ||
                    reason == GroomCoatShadowFallbackReason::NotRequested)
                {
                    continue;
                }
                if (ByReason[i] > 0u)
                {
                    return reason;
                }
            }
            return GroomCoatShadowFallbackReason::None;
        }

        void Reset() noexcept
        {
            *this = GroomCoatShadowStats{};
        }

        [[nodiscard]] auto operator==(const GroomCoatShadowStats&) const -> bool = default;
    };
} // namespace OloEngine
