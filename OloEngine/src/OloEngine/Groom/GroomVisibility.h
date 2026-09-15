#pragma once

// =============================================================================
// GroomVisibility.h — how a strand's sub-pixel coverage becomes a pixel, and
// why it is not the way that was asked for. Issue #1246.
//
// WHY A SEAM AND NOT A BRANCH. A hair strand is thinner than a pixel over most
// of its length, so "did this strand cover this pixel" is a FRACTION, and every
// way of turning that fraction into a frame trades something away: a hard alpha
// cutoff throws the fraction out, alpha-to-coverage quantises it to the sample
// count and needs a multisample target the forward paths do not have, a
// stochastic test keeps it unbiased but spends the error as noise that only a
// temporal resolve removes, and weighted-blended OIT keeps the colour but
// writes no depth at all. Which of those a frame can actually do depends on the
// rendering path, the backend, the sample count and whether TAA/FSR2 is
// running — four unrelated facts. Written as four `if`s in a shader, the
// interesting question ("this groom asked for stochastic coverage and did not
// get it, why?") becomes unanswerable from anywhere.
//
// So, per docs/agent-rules/technique-selection-seams.md, the selection is a
// VALUE: a pure, backend-neutral, constexpr function from what was requested
// plus what the frame actually resolved, to the mode this groom gets and the
// first reason it is not the requested one. It compiles and is tested on a
// machine with no GPU — which is every CI runner here, and which is also the
// machine that takes the fallback arm, so the fallback is the arm CI covers
// best.
//
// WHAT IT DELIBERATELY DOES NOT COVER. It cannot see a wrong ribbon width, a
// depth test configured backwards or a hash that correlates across frames.
// Those are device and arithmetic facts, pinned by GroomCoverage's measured
// error and by the visual-evidence captures — never by this header.
//
// WHY NOT A RenderingPath ROW. Strand composition is orthogonal to the
// G-Buffer strategy: every mode below is meaningful on Forward, Forward+ and
// Deferred, and the two that are not available everywhere are limited by the
// TARGET's sample count, not by the path's identity. Folding it into
// RenderingPath would multiply the enum and would silently change the meaning
// of the ~70 `Path == RenderingPath::Deferred` predicates already in the tree,
// each of which means "is there a G-Buffer". See RenderingPath.h lines 27-42.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The composition modes
    // -------------------------------------------------------------------------

    // How one strand fragment's coverage reaches the frame.
    //
    // Ordered by how much of the coverage fraction survives, NOT by quality:
    // OpaqueRibbon is first because it is the one mode that is always
    // available, and "always available" is the property the fallback chain is
    // built on. A mode is added here only when it changes what the fragment
    // WRITES; a knob that changes how wide the ribbon is, or how many strands
    // are drawn, is a setting and belongs on GroomRenderSettings.
    enum class GroomCompositionMode : u8
    {
        /// Alpha cutoff at a fixed threshold: the fragment is fully there or
        /// fully absent. Writes depth, composes against opaque geometry with
        /// the ordinary depth test, costs nothing extra, and is available on
        /// every backend, path and target. Its silhouette is hard and its
        /// sub-pixel coverage error is whatever the cutoff happens to give —
        /// which is the baseline every other mode is measured against, and the
        /// tier a groom lands on when nothing better can run.
        OpaqueRibbon = 0,

        /// Hashed (stochastic) alpha test: the fragment survives when a
        /// per-pixel, per-frame hash falls under its coverage fraction. The
        /// expectation of the surviving set IS the coverage, so it is unbiased
        /// and order-independent, and it still writes depth — so overlap
        /// against the body and against ordinary opaque geometry stays a plain
        /// depth test. The error is spent as noise, which is only acceptable
        /// when a temporal resolve is running to spend it back.
        StochasticAlpha,

        /// Alpha-to-coverage: the fragment's coverage becomes a sample mask on
        /// a multisample target. Order-independent, depth-writing and
        /// hardware-resolved, but the fraction is quantised to the sample
        /// count, and it needs a multisample target — which in this engine
        /// exists only on the deferred G-Buffer (Renderer3DRenderGraphSetup.cpp
        /// pins the forward scene target to one sample).
        AlphaToCoverage,

        /// Weighted-blended OIT: the fragment joins the engine's existing
        /// accumulation + revealage targets and is composited in
        /// OITResolveRenderPass. Order-independent and path-agnostic, but it
        /// writes NO depth, so nothing drawn afterwards can be occluded by the
        /// strands and the strands cannot be occluded by anything drawn
        /// afterwards either.
        WeightedBlendedOIT,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomCompositionMode mode)
    {
        switch (mode)
        {
            case GroomCompositionMode::OpaqueRibbon:
                return "OpaqueRibbon";
            case GroomCompositionMode::StochasticAlpha:
                return "StochasticAlpha";
            case GroomCompositionMode::AlphaToCoverage:
                return "AlphaToCoverage";
            case GroomCompositionMode::WeightedBlendedOIT:
                return "WeightedBlendedOIT";
            case GroomCompositionMode::Count:
                break;
        }
        return "Unknown";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomCompositionMode(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomCompositionMode::Count);
    }

    // True when the mode leaves a usable depth value behind, so later geometry
    // and the strands themselves compose through the ordinary depth test.
    // Acceptance criterion 2 is stated in exactly these terms, and it is the
    // property that separates the two order-independent modes from each other.
    [[nodiscard]] constexpr bool GroomModeWritesDepth(GroomCompositionMode mode) noexcept
    {
        return mode != GroomCompositionMode::WeightedBlendedOIT;
    }

    // True when the mode's result does not depend on the order the strands are
    // submitted in. Every mode here has this property — stated as a predicate
    // rather than assumed, because a fifth mode (a sorted alpha blend) would
    // not, and the predicate is where that would have to be declared.
    [[nodiscard]] constexpr bool GroomModeIsOrderIndependent(GroomCompositionMode) noexcept
    {
        return true;
    }

    // True when the mode's single-frame output is a stochastic estimate whose
    // error is noise. Such a mode is only honest with a temporal resolve behind
    // it, and SelectGroomComposition refuses it without one.
    [[nodiscard]] constexpr bool GroomModeNeedsTemporalResolve(GroomCompositionMode mode) noexcept
    {
        return mode == GroomCompositionMode::StochasticAlpha;
    }

    // -------------------------------------------------------------------------
    // Segment identity
    // -------------------------------------------------------------------------

    // The stable per-segment number the stochastic hash decorrelates on.
    //
    // It lives here, rather than in either consumer, because THREE places must
    // agree on it: the CPU coverage model (GroomCoverage::ProjectGroom), the
    // vertex data the GPU pass builds (GroomStrandMesh), and therefore the
    // shader that hashes it. Two strands whose ids collided would share a
    // stochastic decision wherever they overlapped, which reads as a coat with
    // correlated holes rather than as noise — a defect that looks like a
    // shading artefact and is really an identity one.
    //
    // The segment index is mixed rather than added so that consecutive
    // segments of one strand are as decorrelated as two different strands.
    [[nodiscard]] inline constexpr u32 GroomSegmentIdentity(u32 curveIndex, u32 segmentIndex) noexcept
    {
        return (curveIndex << 8) ^ (segmentIndex * 2654435761u);
    }

    // -------------------------------------------------------------------------
    // Why it is not what was asked for
    // -------------------------------------------------------------------------

    // Ordered most-fundamental first, in ShadowTechniqueFallbackReason's style:
    // a groom that trips an earlier row would trip later ones too, so the FIRST
    // match is reported and the counter is unambiguous. Every reason is a
    // sentence a human can act on, not a token — these surface in the log, in
    // the Groom inspector and in the renderer statistics panel.
    enum class GroomCompositionFallbackReason : u32
    {
        /// No fallback: the groom got the mode it asked for.
        None = 0,

        /// OpaqueRibbon was requested. Not a failure; it is a deliberate tier,
        /// and counting it as a fallback would report every groom in a scene
        /// that never opted in as a failure.
        NotRequested,

        /// Alpha-to-coverage was requested and the strand pass does not
        /// implement a per-sample coverage mask. Today that is true on BOTH
        /// backends and is a deliberate, recorded outcome of the criterion-1
        /// comparison rather than a device limit — nothing in this engine
        /// sets a sample mask, and the measured evidence did not justify
        /// adding the state to two backends. See
        /// docs/analysis/groom-strand-visibility-1246.md.
        AlphaToCoverageUnimplemented,

        /// Alpha-to-coverage was requested and the target this groom renders
        /// into is single-sample. In this engine that is every forward and
        /// Forward+ frame, and a deferred frame with MSAASampleCount == 1.
        MultisampleTargetUnavailable,

        /// Stochastic alpha was requested and no temporal resolve is running
        /// (TAA off and no temporal upscaler), so the estimate's error would
        /// ship as per-pixel noise. Refused rather than shipped: a noisy coat
        /// is the silent-fallback this repo forbids.
        TemporalResolveUnavailable,

        /// Weighted-blended OIT was requested and the OIT targets were not
        /// produced this frame (RendererSettings::OITEnabled is false, or the
        /// accumulation target does not exist).
        OITTargetsUnavailable,

        /// Weighted-blended OIT was requested by a caller that needs the
        /// strands to leave depth behind. The mode writes none, so honouring
        /// the request would break composition against geometry drawn later.
        DepthCompositionRequired,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomCompositionFallbackReason reason)
    {
        switch (reason)
        {
            case GroomCompositionFallbackReason::None:
                return "the requested strand composition mode is active";
            case GroomCompositionFallbackReason::NotRequested:
                return "opaque ribbons were requested (GroomComponent::CompositionMode)";
            case GroomCompositionFallbackReason::AlphaToCoverageUnimplemented:
                return "the strand pass implements no per-sample coverage mask; the criterion-1 "
                       "comparison measured alpha-to-coverage and did not select it (see "
                       "docs/analysis/groom-strand-visibility-1246.md)";
            case GroomCompositionFallbackReason::MultisampleTargetUnavailable:
                return "alpha-to-coverage needs a multisample target; this frame renders into a "
                       "single-sample one (forward and Forward+ always do, and deferred does at "
                       "MSAASampleCount 1)";
            case GroomCompositionFallbackReason::TemporalResolveUnavailable:
                return "stochastic coverage needs a temporal resolve to converge; TAA is off and no "
                       "temporal upscaler is running, so its error would ship as per-pixel noise";
            case GroomCompositionFallbackReason::OITTargetsUnavailable:
                return "weighted-blended OIT needs the accumulation and revealage targets; they were "
                       "not produced this frame (RendererSettings::OITEnabled)";
            case GroomCompositionFallbackReason::DepthCompositionRequired:
                return "weighted-blended OIT writes no depth, and this groom must compose against "
                       "geometry drawn after it";
            case GroomCompositionFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // What the frame actually resolved
    // -------------------------------------------------------------------------

    // The gathered facts the selection is a function of. Every field is
    // something the frame RESOLVED, never something it expects to: the sample
    // count is the target's, not the setting's, and TemporalResolveActive is
    // true only once a resolve pass is genuinely running. Reading a request
    // here instead of a result is how a fallback becomes invisible.
    struct GroomCompositionInputs
    {
        /// What the groom asked for.
        GroomCompositionMode Requested = GroomCompositionMode::OpaqueRibbon;

        /// Sample count of the colour target this groom renders into. 1 means
        /// single-sample. Taken from the framebuffer, not from
        /// DeferredSettings::MSAASampleCount, because the latter is clamped to
        /// the device at G-Buffer creation and can differ.
        u32 TargetSampleCount = 1;

        /// Whether the strand pass can set a per-sample coverage mask on this
        /// backend. False everywhere today; the field exists so the refusal is
        /// a gathered input like every other, and so implementing it later is
        /// one producer change rather than a new branch.
        bool AlphaToCoverageSupported = false;

        /// Whether a temporal resolve (engine TAA or a temporal upscaler) is
        /// running this frame and will consume this pass's output.
        bool TemporalResolveActive = false;

        /// Whether the OIT accumulation and revealage targets exist this frame.
        bool OITTargetsAvailable = false;

        /// Whether this groom's caller needs the strands to leave depth behind.
        /// True for any groom in an ordinary scene — geometry is drawn after
        /// the strand pass — and the field exists so a future strand-only
        /// capture can say otherwise rather than having the answer assumed.
        bool RequiresDepthComposition = true;
    };

    // The decision, and everything needed to explain it.
    struct GroomCompositionDecision
    {
        GroomCompositionMode Effective = GroomCompositionMode::OpaqueRibbon;
        GroomCompositionFallbackReason Reason = GroomCompositionFallbackReason::NotRequested;

        /// Sample count the effective mode will actually use. Equal to the
        /// target's for AlphaToCoverage and 1 for every other mode, so a
        /// caller sizing a sample-mask table reads this rather than
        /// re-deriving it from the mode.
        u32 EffectiveSampleCount = 1;

        [[nodiscard]] constexpr bool IsFallback() const noexcept
        {
            return Reason != GroomCompositionFallbackReason::None
                   && Reason != GroomCompositionFallbackReason::NotRequested;
        }

        [[nodiscard]] auto operator==(const GroomCompositionDecision&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    // Pure, constexpr, backend-neutral. Reads nothing but `inputs`.
    //
    // Every refusal falls back to OpaqueRibbon rather than to the "next best"
    // mode. That is deliberate: a chain of partial degradations gives a groom
    // whose appearance depends on three facts nobody looked at, whereas one
    // always-available tier plus a named reason gives a picture a human can
    // recognise and a counter they can act on.
    [[nodiscard]] constexpr GroomCompositionDecision SelectGroomComposition(const GroomCompositionInputs& inputs) noexcept
    {
        GroomCompositionDecision decision;

        switch (inputs.Requested)
        {
            case GroomCompositionMode::OpaqueRibbon:
                decision.Effective = GroomCompositionMode::OpaqueRibbon;
                decision.Reason = GroomCompositionFallbackReason::NotRequested;
                return decision;

            case GroomCompositionMode::AlphaToCoverage:
                if (!inputs.AlphaToCoverageSupported)
                {
                    decision.Reason = GroomCompositionFallbackReason::AlphaToCoverageUnimplemented;
                    return decision;
                }
                if (inputs.TargetSampleCount < 2u)
                {
                    decision.Reason = GroomCompositionFallbackReason::MultisampleTargetUnavailable;
                    return decision;
                }
                decision.Effective = GroomCompositionMode::AlphaToCoverage;
                decision.Reason = GroomCompositionFallbackReason::None;
                decision.EffectiveSampleCount = inputs.TargetSampleCount;
                return decision;

            case GroomCompositionMode::StochasticAlpha:
                if (!inputs.TemporalResolveActive)
                {
                    decision.Reason = GroomCompositionFallbackReason::TemporalResolveUnavailable;
                    return decision;
                }
                decision.Effective = GroomCompositionMode::StochasticAlpha;
                decision.Reason = GroomCompositionFallbackReason::None;
                return decision;

            case GroomCompositionMode::WeightedBlendedOIT:
                if (!inputs.OITTargetsAvailable)
                {
                    decision.Reason = GroomCompositionFallbackReason::OITTargetsUnavailable;
                    return decision;
                }
                if (inputs.RequiresDepthComposition)
                {
                    decision.Reason = GroomCompositionFallbackReason::DepthCompositionRequired;
                    return decision;
                }
                decision.Effective = GroomCompositionMode::WeightedBlendedOIT;
                decision.Reason = GroomCompositionFallbackReason::None;
                return decision;

            case GroomCompositionMode::Count:
                break;
        }

        // An out-of-range request is corruption, not a preference: fall to the
        // always-available tier and report it as unsupported rather than
        // pretending the groom asked for the baseline.
        decision.Reason = GroomCompositionFallbackReason::AlphaToCoverageUnimplemented;
        return decision;
    }

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    // What a user reads when the coat does not look the way they set it up to.
    // "Failed to deliver" and "never asked" are separate on purpose: a scene
    // full of OpaqueRibbon grooms is not a scene full of failures.
    struct GroomCompositionStats
    {
        u32 GroomsConsidered = 0;
        u32 GroomsOnRequestedMode = 0;
        u32 GroomsFellBack = 0;
        std::array<u32, static_cast<sizet>(GroomCompositionFallbackReason::Count)> ByReason{};

        void Record(const GroomCompositionDecision& decision) noexcept
        {
            ++GroomsConsidered;
            if (decision.IsFallback())
            {
                ++GroomsFellBack;
            }
            else
            {
                ++GroomsOnRequestedMode;
            }
            ByReason[static_cast<sizet>(decision.Reason)] += 1u;
        }

        void Reset() noexcept
        {
            *this = GroomCompositionStats{};
        }

        // The reason to show when only one line fits. Skips None and
        // NotRequested — neither is something to fix — and returns the most
        // common genuine fallback, breaking ties toward the more fundamental
        // (lower) reason so the answer names a cause rather than a symptom.
        [[nodiscard]] GroomCompositionFallbackReason DominantFallbackReason() const noexcept
        {
            auto dominant = GroomCompositionFallbackReason::None;
            u32 best = 0;
            for (sizet i = static_cast<sizet>(GroomCompositionFallbackReason::NotRequested) + 1u;
                 i < ByReason.size(); ++i)
            {
                if (ByReason[i] > best)
                {
                    best = ByReason[i];
                    dominant = static_cast<GroomCompositionFallbackReason>(i);
                }
            }
            return dominant;
        }

        [[nodiscard]] auto operator==(const GroomCompositionStats&) const -> bool = default;
    };
} // namespace OloEngine
