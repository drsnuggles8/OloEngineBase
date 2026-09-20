#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomLod.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Authoring bounds. REJECTION-then-default, never a silent clamp of a
        // NaN: a non-finite threshold compared with `<` is false in both
        // directions, which would pin a coat on one tier forever with nothing
        // in any log — the silent failure this repo forbids.
        constexpr f32 kMinPixelThreshold = 0.5f;
        constexpr f32 kMaxPixelThreshold = 16384.0f;
        constexpr f32 kMaxHysteresis = 0.5f;
        constexpr f32 kMaxWidthCompensation = 32.0f;
        constexpr u32 kMaxHoldFrames = 600;
        constexpr u32 kMaxBudgetSteps = 16;

        [[nodiscard]] f32 SanitizePixelThreshold(f32 value, f32 fallback) noexcept
        {
            if (!std::isfinite(value) || value <= 0.0f)
            {
                return fallback;
            }
            return std::clamp(value, kMinPixelThreshold, kMaxPixelThreshold);
        }

        [[nodiscard]] GroomLodBudgetCurve SanitizeCurve(const GroomLodBudgetCurve& curve,
                                                        const GroomLodBudgetCurve& fallback) noexcept
        {
            GroomLodBudgetCurve out;
            out.FullPixelSize = SanitizePixelThreshold(curve.FullPixelSize, fallback.FullPixelSize);
            out.MaxSteps = std::min(curve.MaxSteps, kMaxBudgetSteps);
            return out;
        }

        // The tier `pixelSize` selects, with the thresholds slid so as to HOLD
        // whatever `current` already is.
        //
        // The slide is what makes a dead band: coarsening past an edge needs
        // the coat to shrink an extra `h` below it, and refining back needs it
        // to grow an extra `h` above. So the band a transition can happen in is
        // `2 * h * threshold` wide, and an oscillation NARROWER than that band
        // cannot contain both edges — it therefore causes at most ONE
        // transition, ever, whatever its period. That is the guarantee
        // GroomLodContractTest asserts, and it is deliberately stated as a
        // bound rather than as "no flicker": an oscillation wider than the band
        // still crosses, and the frame hold below is what bounds THAT case.
        [[nodiscard]] GroomRepresentation RepresentationAt(const GroomLodPolicy& policy, f32 pixelSize,
                                                           GroomRepresentation current) noexcept
        {
            const f32 h = policy.Hysteresis;
            const f32 coarsen = 1.0f - h;
            const f32 refine = 1.0f + h;

            const f32 cardEdge =
                policy.CardPixelSize * (current <= GroomRepresentation::Strand ? coarsen : refine);
            // min() against the card edge: an author who set the two thresholds
            // close together, plus a wide hysteresis, could otherwise put the
            // mesh edge ABOVE the card edge and make the ladder non-monotone —
            // a coat would skip the card tier on the way out and take it on the
            // way back. Every stability argument in this file assumes the
            // ladder is ordered, so it is made ordered here rather than assumed.
            const f32 meshEdge =
                std::min(policy.MeshPixelSize * (current <= GroomRepresentation::Card ? coarsen : refine), cardEdge);

            if (pixelSize < meshEdge)
            {
                return GroomRepresentation::Mesh;
            }
            if (pixelSize < cardEdge)
            {
                return GroomRepresentation::Card;
            }
            return GroomRepresentation::Strand;
        }

        // Refining is immediate, coarsening waits for the hold.
        //
        // The asymmetry is GroomCoatShadow::ApplyCoatLodHysteresis's, for its
        // reason: a coat that just got bigger and is still on the coarse tier
        // is visibly wrong, while one that stays on the fine tier a few frames
        // too long is merely expensive, and only one of those two is a picture
        // the user can see.
        template<typename T>
        [[nodiscard]] T ApplyHold(T current, T requested, u32 framesStable, u32 holdFrames) noexcept
        {
            if (requested == current || requested < current)
            {
                return requested;
            }
            return framesStable >= holdFrames ? requested : current;
        }

        // One budget axis, advanced by a frame. Returns the step to USE.
        //
        // THE DEAD BAND IS APPLIED TO THE SIZE, NOT TO THE THRESHOLDS, because
        // the thresholds are a halving ladder rather than two numbers: asking
        // the ladder what an inflated coat wants, and separately what a
        // deflated one wants, gives the same "hold what you have" band at every
        // rung with one function instead of a per-rung pair.
        [[nodiscard]] u32 AdvanceBudgetStep(const GroomLodBudgetCurve& curve, f32 pixelSize, f32 hysteresis,
                                            u32 holdFrames, u32& current, u32& requested, u32& stableFrames) noexcept
        {
            const u32 coarserWant = GroomLodBudgetStep(curve, pixelSize * (1.0f + hysteresis));
            const u32 finerWant = GroomLodBudgetStep(curve, pixelSize * (1.0f - hysteresis));

            u32 want = current;
            if (coarserWant > current)
            {
                want = coarserWant;
            }
            else if (finerWant < current)
            {
                want = finerWant;
            }

            stableFrames = (want == requested) ? stableFrames + 1u : 0u;
            requested = want;

            current = ApplyHold(current, want, stableFrames, holdFrames);
            return current;
        }
    } // namespace

    u32 GroomLodBudgetStep(const GroomLodBudgetCurve& curve, f32 pixelSize) noexcept
    {
        // A coat with no measurable apparent size is behind the camera or
        // degenerate. The cheapest step is the honest answer: spending a full
        // budget on something nobody can see is the failure this whole file is
        // about, and the counter that would explain it is already the one the
        // caller reports.
        if (!std::isfinite(pixelSize) || pixelSize <= 0.0f)
        {
            return curve.MaxSteps;
        }
        if (!std::isfinite(curve.FullPixelSize) || curve.FullPixelSize <= 0.0f)
        {
            return 0;
        }
        if (pixelSize >= curve.FullPixelSize)
        {
            return 0;
        }

        u32 step = 0;
        f32 threshold = curve.FullPixelSize;
        while (step < curve.MaxSteps)
        {
            threshold *= 0.5f;
            ++step;
            if (pixelSize >= threshold)
            {
                break;
            }
        }
        return step;
    }

    f32 GroomLodWidthCompensation(f32 achievedFraction, f32 maxScale) noexcept
    {
        const f32 cap = (std::isfinite(maxScale) && maxScale > 1.0f) ? std::min(maxScale, kMaxWidthCompensation) : 1.0f;
        if (!std::isfinite(achievedFraction) || achievedFraction <= 0.0f)
        {
            // Nothing was retained, so there is nothing to widen. Identity
            // rather than the cap: a caller in this state already reports
            // StrandsSelected == 0, and returning a large multiplier would make
            // an EMPTY coat look like a deliberately fat one the moment a
            // single strand came back.
            return 1.0f;
        }
        if (achievedFraction >= 1.0f)
        {
            return 1.0f;
        }
        return std::clamp(1.0f / achievedFraction, 1.0f, cap);
    }

    GroomLodPolicy SanitizeGroomLodPolicy(const GroomLodPolicy& policy) noexcept
    {
        const GroomLodPolicy defaults;
        GroomLodPolicy out;

        out.Enabled = policy.Enabled;
        out.CardPixelSize = SanitizePixelThreshold(policy.CardPixelSize, defaults.CardPixelSize);
        out.MeshPixelSize = SanitizePixelThreshold(policy.MeshPixelSize, defaults.MeshPixelSize);
        // THE ORDERING IS ENFORCED, NOT ASSUMED. An inverted pair makes the
        // ladder non-monotone, and every stability statement in this file rests
        // on its being ordered.
        out.MeshPixelSize = std::min(out.MeshPixelSize, out.CardPixelSize);

        out.Hysteresis =
            std::isfinite(policy.Hysteresis) ? std::clamp(policy.Hysteresis, 0.0f, kMaxHysteresis) : defaults.Hysteresis;
        out.MaxWidthCompensation = std::isfinite(policy.MaxWidthCompensation)
                                       ? std::clamp(policy.MaxWidthCompensation, 1.0f, kMaxWidthCompensation)
                                       : defaults.MaxWidthCompensation;
        out.HoldFrames = std::min(policy.HoldFrames, kMaxHoldFrames);

        out.Visibility = SanitizeCurve(policy.Visibility, defaults.Visibility);
        out.Simulation = SanitizeCurve(policy.Simulation, defaults.Simulation);
        out.Shadow = SanitizeCurve(policy.Shadow, defaults.Shadow);
        return out;
    }

    GroomLodDecision IdentityGroomLodDecision() noexcept
    {
        GroomLodDecision decision;
        decision.Representation = GroomRepresentation::Strand;
        decision.Reason = GroomLodFallbackReason::NotRequested;
        decision.VisibilityStep = 0;
        decision.SimulationStep = 0;
        decision.ShadowStep = 0;
        decision.VisibilityFraction = 1.0f;
        decision.SimulationFraction = 1.0f;
        decision.PixelSize = 0.0f;
        return decision;
    }

    GroomLodDecision AdvanceGroomLod(const GroomLodPolicy& rawPolicy, const GroomLodInputs& inputs,
                                     GroomLodState& state) noexcept
    {
        const GroomLodPolicy policy = SanitizeGroomLodPolicy(rawPolicy);
        const f32 pixelSize = std::isfinite(inputs.PixelSize) ? std::max(inputs.PixelSize, 0.0f) : 0.0f;

        if (!policy.Enabled)
        {
            // THE STATE IS RESET, not merely ignored. Leaving the hysteresis
            // counters where they were would mean that turning LOD off and on
            // again resumed from a tier chosen at a camera position that no
            // longer exists — and the A/B control for every capture in this
            // issue is "turn it off", which has to be the same picture every
            // time it is taken.
            state = GroomLodState{};
            GroomLodDecision decision = IdentityGroomLodDecision();
            decision.PixelSize = pixelSize;
            return decision;
        }

        GroomLodDecision decision;
        decision.PixelSize = pixelSize;

        // ── Representation ──────────────────────────────────────────────
        const GroomRepresentation ideal = RepresentationAt(policy, pixelSize, state.Representation);
        state.RepresentationStableFrames =
            (ideal == state.RequestedRepresentation) ? state.RepresentationStableFrames + 1u : 0u;
        state.RequestedRepresentation = ideal;

        // What the cook and the renderer can actually honour. Walked from the
        // ideal tier toward the finest one, so the FIRST refusal is the one
        // reported — the enum is ordered most-fundamental-first for exactly
        // that reason, and a coat missing both its levels names the cook rather
        // than the hold.
        auto reason = GroomLodFallbackReason::None;
        GroomRepresentation target = ideal;
        if (target == GroomRepresentation::Mesh)
        {
            if (!inputs.MeshTierSupported)
            {
                reason = GroomLodFallbackReason::MeshTierNotSelected;
                target = GroomRepresentation::Card;
            }
            else if (!inputs.MeshLevelAvailable)
            {
                reason = GroomLodFallbackReason::LevelNotCooked;
                target = GroomRepresentation::Card;
            }
        }
        if (target == GroomRepresentation::Card && !inputs.CardLevelAvailable)
        {
            if (reason == GroomLodFallbackReason::None)
            {
                reason = GroomLodFallbackReason::LevelNotCooked;
            }
            target = GroomRepresentation::Strand;
        }

        const GroomRepresentation applied =
            ApplyHold(state.Representation, target, state.RepresentationStableFrames, policy.HoldFrames);
        if (applied != target && reason == GroomLodFallbackReason::None)
        {
            // Only when nothing more fundamental already explains it: a coat
            // whose card level was never cooked is not "held by hysteresis",
            // and sending its owner to the hold setting would waste their time.
            reason = GroomLodFallbackReason::HeldByHysteresis;
        }
        decision.RepresentationChanged = applied != state.Representation;
        state.Representation = applied;
        decision.Representation = applied;
        decision.Reason = reason;

        // ── The three budgets, each on its own curve and its own hold ────
        //
        // Three calls, three independent pieces of state. Criterion 3 asks for
        // them to scale down independently, and sharing one stability counter
        // would recouple them through the hysteresis even though their curves
        // are separate: a shadow step that kept flickering would pin the
        // visibility step at its old value.
        decision.VisibilityStep =
            AdvanceBudgetStep(policy.Visibility, pixelSize, policy.Hysteresis, policy.HoldFrames, state.VisibilityStep,
                              state.RequestedVisibilityStep, state.VisibilityStableFrames);
        decision.SimulationStep =
            AdvanceBudgetStep(policy.Simulation, pixelSize, policy.Hysteresis, policy.HoldFrames, state.SimulationStep,
                              state.RequestedSimulationStep, state.SimulationStableFrames);
        decision.ShadowStep =
            AdvanceBudgetStep(policy.Shadow, pixelSize, policy.Hysteresis, policy.HoldFrames, state.ShadowStep,
                              state.RequestedShadowStep, state.ShadowStableFrames);

        decision.VisibilityFraction = GroomLodStepFraction(decision.VisibilityStep);
        decision.SimulationFraction = GroomLodStepFraction(decision.SimulationStep);
        return decision;
    }
} // namespace OloEngine
