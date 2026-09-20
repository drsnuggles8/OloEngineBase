#pragma once

// =============================================================================
// GroomLod.h — which representation a groom is drawn as, how much work each of
// its three budgets gets, and why the answer does not change every frame.
// Issue #1252.
//
// WHY A SEAM AND NOT THREE `if`s IN THE PASS. A coat at a distance has three
// unrelated costs — the ribbons it rasterises, the guides it integrates, and
// the shadow volume it bakes — and criterion 3 is explicit that they scale down
// INDEPENDENTLY. One coupled "LOD scalar" satisfies none of it: a coat whose
// simulation was too expensive would also lose its silhouette, and a coat whose
// shadow volume was the problem would lose its guides. So each axis gets its own
// curve, each carries its own hysteresis state, and the whole decision is a
// VALUE — a pure function from apparent size plus what the cook produced, to the
// tier this groom gets and the first reason it is not the tier distance asked
// for. Same discipline, same reason, as GroomVisibility.h and
// GroomCoatShadowTechnique.h: it compiles and is tested on a machine with no
// GPU, and the interesting question ("this coat is on cards at two metres,
// why?") is answerable from a counter rather than from a debugger.
//
// WHAT "APPARENT SIZE" IS HERE. Pixels of the render target's HEIGHT, from
// EstimateProjectedPixelSize (Renderer/LOD.h) against Renderer3D's LOD view —
// the same orientation-independent projection, from the same CULLING camera,
// that every other LOD in this engine uses (issue #726). Deliberately not a
// distance: a threshold in metres is wrong at the next field of view and wrong
// again at 4K, and a camera-plane projection makes a coat pop when the camera
// merely rotates in place.
//
// THE COVERAGE CONTRACT, AND WHY THE COMPENSATION IS LINEAR.
//
// Thinning a coat to a fraction k of its strands removes a fraction (1 - k) of
// its apparent density. A strand is a BAND: the area it covers is its projected
// length times its projected width, so the coat's covered area is proportional
// to count x width. Halving the count and doubling the width therefore restores
// it exactly, and the compensation is 1/k — LINEAR, not the 1/sqrt(k) that
// FoliageLod::CoverageCompensation uses. Foliage's instances are sprites whose
// area goes as the SQUARE of their linear size, so the two differ by a square
// root, and using foliage's factor here under-compensates a 16x thinning by 4x.
// GroomLodContractTest asserts the relation over a sweep rather than at a point.
//
// IT KEEPS WORKING BELOW ONE PIXEL, which is where hair actually lives (see
// groom-strand-visibility.md rule 1). A sub-pixel strand is rasterised at one
// pixel and carries `oloGroomWidenedAlpha` = trueHalfWidth / halfPixel as its
// alpha; doubling the true width doubles that alpha and leaves the rasterised
// width alone. So the same 1/k factor preserves coverage through the alpha lane
// below a pixel and through the geometry above it, with no branch anywhere.
//
// AND IT IS COMPUTED FROM THE ACHIEVED FRACTION, NEVER THE REQUESTED ONE — see
// GroomLodWidthCompensation, which has that warning attached to it, because the
// strand budget is spent as an integer STRIDE PER ROLE and the fraction it
// actually retains is not the fraction that was asked for.
//
// WHY THE BUDGETS ARE HALVINGS AND THE COMPENSATION IS CONTINUOUS. The strand
// geometry is cached, keyed on the build settings, so a budget that slid
// continuously with the camera would rebuild every groom's vertex buffer every
// frame — the exact cost the cache exists to remove. A budget that moves in
// halvings rebuilds a handful of times over a whole approach. The pop that
// would leave at each step is removed by the width compensation, which is a UBO
// value and costs nothing to change: at the instant the stride doubles the
// compensation doubles with it, and the coat's total coverage does not move.
// What remains at a step is SPATIAL — fewer, fatter strands — and that is what
// the distance thresholds bound and what the evidence captures show.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <array>
#include <string_view>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The representation ladder
    // -------------------------------------------------------------------------

    // How a groom's geometry is expressed at this distance.
    //
    // Ordered FINEST FIRST, so "coarser" is "greater" and the hysteresis below
    // can say `requested < current` for a refinement exactly the way
    // GroomCoatShadow::ApplyCoatLodHysteresis does for its resolution steps.
    //
    // EVERY TIER IS DRAWN BY THE SAME PASS AND THE SAME SHADER. A card is a
    // cooked CURVE like a strand is, so the fibre BCSDF (#1247), the coat
    // shadow volume (#1248), the per-strand tint (#1251) and the guide
    // simulation (#1250) all keep working across a transition without a second
    // implementation that could disagree with the first. That is not a
    // convenience: criterion 1 asks for colour and highlight response to be
    // PRESERVED across transitions, and the cheapest way to be sure of that is
    // for there to be only one shading path.
    enum class GroomRepresentation : u8
    {
        /// Every cooked strand the budget can afford. The close-up target, and
        /// the tier the scope boundary says nothing may replace.
        Strand = 0,

        /// One ribbon per CLUMP: the cluster's most central real strand, kept
        /// verbatim, widened to carry its members' summed width. A card in the
        /// sense that matters — one quad strip where a tuft was — and
        /// deliberately not in the sense of an authored alpha texture, because
        /// a baked card texture fixes the coat's shading at bake time and the
        /// whole point of routing the aggregate through WIDTH (and therefore
        /// through the widened alpha) is that the live BCSDF still lights it.
        ///
        /// WHY IT IS COOKED RATHER THAN LEFT TO THE BUDGET. Thinning by k and
        /// widening by 1/k is the same arithmetic and it is free — but the
        /// runtime widening is capped (MaxWidthCompensation), because a strand
        /// widened sixty times is a flat band rather than a fibre. Past the cap
        /// a stride cannot restore the density at all: at a 59x reduction the
        /// measured strand arm carries 0.23 of the coat's area and the card
        /// carries 1.01. The card's width is BAKED, so the cap does not apply
        /// to it. See docs/analysis/groom-representation-lod-1252.md.
        Card = 1,

        /// A shell: the coat as a closed surface rather than as fibres. The
        /// issue's "mesh" tier. Only honest where the true coverage has
        /// saturated — a shell claims a coverage of 1 everywhere inside the
        /// silhouette, so a coat you can see through reads as a solid lump.
        /// Whether any of this engine's reference coats reach that state at a
        /// plausible distance is a MEASUREMENT, recorded in
        /// docs/analysis/groom-representation-lod-1252.md, not an assumption.
        Mesh = 2,

        Count
    };

    constexpr u32 GroomRepresentationCount = static_cast<u32>(GroomRepresentation::Count);

    [[nodiscard]] constexpr std::string_view ToString(GroomRepresentation representation) noexcept
    {
        switch (representation)
        {
            case GroomRepresentation::Strand:
                return "Strand";
            case GroomRepresentation::Card:
                return "Card";
            case GroomRepresentation::Mesh:
                return "Mesh";
            case GroomRepresentation::Count:
                break;
        }
        return "Strand";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomRepresentation(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomRepresentation::Count);
    }

    // -------------------------------------------------------------------------
    // Why it is not the tier distance asked for
    // -------------------------------------------------------------------------
    // Ordered most-fundamental first, in GroomCompositionFallbackReason's
    // style: a groom that trips an earlier row would trip later ones too, so
    // the FIRST match is reported and the counter is unambiguous.
    enum class GroomLodFallbackReason : u32
    {
        /// The groom is on the tier its apparent size asks for.
        None = 0,

        /// This groom has no LOD policy enabled. Not a failure and deliberately
        /// first among the non-reasons: counting every groom in a scene that
        /// never opted in as a fallback saturates the counter that explains a
        /// coat which did.
        NotRequested,

        /// The apparent size asks for a coarser tier and the cook produced no
        /// level for it. The fix is a re-cook, not a setting, which is why it
        /// is its own reason rather than folded into the one below.
        LevelNotCooked,

        /// The apparent size asks for the Mesh tier and the shell is not a tier
        /// this engine ships. The measured comparison is in
        /// docs/analysis/groom-representation-lod-1252.md; a coat that trips
        /// this is drawn on cards, which is a picture rather than a hole.
        MeshTierNotSelected,

        /// The apparent size asks for a COARSER tier, the level exists, and the
        /// request has not been stable long enough. Transient by construction —
        /// a counter that stays high means a camera is straddling a threshold,
        /// which is the thrashing criterion 2 names, caught as a number before
        /// it is visible as a picture.
        HeldByHysteresis,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomLodFallbackReason reason) noexcept
    {
        switch (reason)
        {
            case GroomLodFallbackReason::None:
                return "the groom is on the representation its apparent size selects";
            case GroomLodFallbackReason::NotRequested:
                return "representation LOD is disabled on this groom (GroomLodComponent)";
            case GroomLodFallbackReason::LevelNotCooked:
                return "the apparent size selects a coarser representation and the cooked groom "
                       "carries no level for it; re-cook the groom with that level enabled";
            case GroomLodFallbackReason::MeshTierNotSelected:
                return "the apparent size selects the shell tier, which the measured comparison did "
                       "not select (see docs/analysis/groom-representation-lod-1252.md)";
            case GroomLodFallbackReason::HeldByHysteresis:
                return "a coarser representation is available and the request has not been stable "
                       "for the policy's hold; the coat is holding the tier it has";
            case GroomLodFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // The budget curve
    // -------------------------------------------------------------------------

    // One axis's policy: how much of its work survives at a given apparent size.
    //
    // A STEP IS A HALVING, and the shape is deliberately the one
    // GroomCoatShadow::CoatLodPolicy already uses, so this engine has ONE idiom
    // for "how far down the ladder is this coat" rather than two that drift.
    //
    // THE RUNG RULE, stated the way the loop actually walks it: halve
    // `FullPixelSize` until the coat is at least as large as the rung, and the
    // number of halvings taken is the step. So step k is selected for a coat in
    // `[FullPixelSize / 2^k, FullPixelSize / 2^(k-1))` — at a 512 px full size,
    // 100 px is step 3 (it clears the 64 px rung, not the 128 px one), NOT the
    // step 2 that "below FullPixelSize / 2^k" would suggest. That off-by-one
    // phrasing is easy to write down and wrong; GroomLodBudget's sweep is what
    // pins the real rule. See the file header for why the steps are discrete
    // and the compensation that hides them is not.
    struct GroomLodBudgetCurve
    {
        /// Apparent size, in pixels of the render target's height, at and above
        /// which this axis runs at full rate.
        f32 FullPixelSize = 512.0f;

        /// Halvings allowed. 4 is a sixteenth of the work at the far end.
        u32 MaxSteps = 4;

        /// Bit-exact, per cpp-coding-quality §2a: a defaulted operator== on a
        /// struct holding a float is a float `==`, which this repo forbids.
        [[nodiscard]] auto operator==(const GroomLodBudgetCurve& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomLodBudgetCurve) == 8,
                  "GroomLodBudgetCurve is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomLodBudgetCurve>);

    // The step this curve asks for at `pixelSize`.
    //
    // PURE AND MONOTONE NON-DECREASING as the coat shrinks: a coat that gets
    // smaller never asks for MORE work. That is the property an oscillation
    // argument rests on, and GroomLodContractTest sweeps it rather than
    // sampling it.
    [[nodiscard]] u32 GroomLodBudgetStep(const GroomLodBudgetCurve& curve, f32 pixelSize) noexcept;

    // The fraction of full rate a step runs at: 2^-step.
    [[nodiscard]] constexpr f32 GroomLodStepFraction(u32 step) noexcept
    {
        // Clamped at 30 so the shift is defined for any u32 a corrupt policy
        // could produce; 2^-30 is already a billionth of the work.
        const u32 clamped = step < 30u ? step : 30u;
        return 1.0f / static_cast<f32>(1u << clamped);
    }

    // The linear width multiplier that restores a thinned coat's apparent
    // density, capped by the policy.
    //
    // `achievedFraction` MUST BE THE FRACTION THE BUILD ACTUALLY RETAINED, not
    // the one the policy asked for. The strand budget is spent as an integer
    // stride per role (GroomStrandMesh.cpp), so asking for 0.4 of a role
    // retains 1/3 of it; compensating by 1/0.4 then leaves the coat a sixth
    // thinner than it started and the error grows with every step. The pass
    // therefore plans the build, reads StrandsSelected / StrandsAvailable back
    // out of the stats, and compensates on THAT.
    //
    // See the file header for why this is 1/k and not 1/sqrt(k).
    [[nodiscard]] f32 GroomLodWidthCompensation(f32 achievedFraction, f32 maxScale) noexcept;

    // -------------------------------------------------------------------------
    // The authored policy
    // -------------------------------------------------------------------------

    // Everything a groom's LOD is authored with. Arrives from scene YAML, from
    // a save game, from the editor or from an MCP write, each of which is an
    // untrusted float per CLAUDE.md — so every consumer builds through
    // SanitizeGroomLodPolicy and nothing reads these fields raw.
    struct GroomLodPolicy
    {
        // MEMBERS ARE ORDERED 4-BYTE THEN 1-BYTE with the tail padded
        // explicitly, because operator== below is a whole-object memcmp and a
        // padded layout compares unequal when logically equal (issue #1019).
        // BitwiseEqualLayoutTest lists this type for exactly that reason.

        // ── Representation thresholds ────────────────────────────────
        //
        // Apparent size, in pixels of the render target's height, BELOW which
        // the named tier is selected. Ordered: a coat larger than
        // CardPixelSize is on strands, one between the two is on cards, one
        // below MeshPixelSize is on the shell.

        f32 CardPixelSize = 256.0f;
        f32 MeshPixelSize = 48.0f;

        // ── Anti-thrash ──────────────────────────────────────────────

        /// Fraction the held tier's threshold slides by. The band moves so as
        /// to HOLD what the coat already has: on strands you must shrink past
        /// CardPixelSize * (1 - h) to hand over, and on cards you must grow
        /// past CardPixelSize * (1 + h) to come back. ONE offset applied to
        /// both edges, for FoliageLod::HysteresisOffset's reason — the band
        /// must move bodily, not change width.
        f32 Hysteresis = 0.15f;

        /// Cap on GroomLodWidthCompensation. Past about 8x a strand is no
        /// longer a strand — it is a wide flat band that reads as a different
        /// material — so beyond the cap the coat genuinely does thin out, and
        /// the editor shows the achieved compensation next to the cap rather
        /// than clamping it silently.
        f32 MaxWidthCompensation = 8.0f;

        /// Consecutive frames a COARSER request must persist before it is
        /// taken. Refining is immediate: a coat that just got bigger and is
        /// still on cards is visibly wrong, while one that stays on strands a
        /// few frames too long is merely expensive, and only one of those is a
        /// picture anyone can see. Same rule, same justification, as
        /// GroomCoatShadow::ApplyCoatLodHysteresis.
        ///
        /// THIS IS THE MECHANISM FoliageLod DOES NOT HAVE, and it is why the
        /// guarantee here is stronger than that file's. Foliage carries no
        /// per-instance state, so its only memory is one frame of camera
        /// position and it cannot beat a two-frame oscillation. A groom is an
        /// ENTITY: its LOD state persists in Scene, so a camera oscillating
        /// across a threshold every frame changes this coat's representation at
        /// most once every HoldFrames frames, whatever its amplitude.
        u32 HoldFrames = 4;

        // ── The three independent budgets (criterion 3) ──────────────

        /// Ribbon geometry: how many strands are built and drawn.
        GroomLodBudgetCurve Visibility{ 512.0f, 4u };
        /// Guide simulation: how many guides are integrated.
        ///
        /// Its own curve, and authored to start earlier than visibility by
        /// default. Motion is read at a coarser spatial scale than silhouette
        /// — a coat whose guides thin out looks the same until it moves, while
        /// one whose strands thin out is visible standing still.
        GroomLodBudgetCurve Simulation{ 384.0f, 4u };
        /// Coat self-shadow volume resolution: added to the step
        /// GroomCoatShadow::CoatLodPolicy already selects, so this BIASES that
        /// policy rather than replacing it.
        GroomLodBudgetCurve Shadow{ 512.0f, 3u };

        /// False leaves the groom exactly as it was before this issue: every
        /// strand the static budget affords, at every distance. The A/B control
        /// for every capture is "turn this off".
        bool Enabled = false;
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const GroomLodPolicy& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomLodPolicy) == 48,
                  "GroomLodPolicy is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomLodPolicy>);

    /// Replaces every non-finite or out-of-range field with a usable value.
    /// Never rejects a policy — a bad number takes the neutral answer, which
    /// for every field here is the engine default rather than "off", because a
    /// coat that silently stopped LODding is a performance cliff nobody sees.
    /// The one ordering invariant (MeshPixelSize <= CardPixelSize) is enforced
    /// rather than assumed: an inverted pair would make the ladder
    /// non-monotone and every stability argument here false.
    [[nodiscard]] GroomLodPolicy SanitizeGroomLodPolicy(const GroomLodPolicy& policy) noexcept;

    // -------------------------------------------------------------------------
    // What the frame resolved
    // -------------------------------------------------------------------------

    // Every field is a RESULT, never a request — the same rule
    // GroomCompositionInputs states. `CardLevelAvailable` is whether the COOKED
    // asset carries the level, not whether anyone wanted it.
    struct GroomLodInputs
    {
        /// Apparent size in pixels of the render target's height, from
        /// EstimateProjectedPixelSize against Renderer3D::GetLODViewParams().
        f32 PixelSize = 0.0f;

        /// Whether the cooked groom carries a level for each coarser tier.
        bool CardLevelAvailable = false;
        bool MeshLevelAvailable = false;

        /// Whether the shell tier is one the renderer can draw at all. False
        /// in every build today; the field exists so the refusal is a gathered
        /// input like every other, and so shipping it later is one producer
        /// change rather than a new branch. See
        /// docs/analysis/groom-representation-lod-1252.md.
        bool MeshTierSupported = false;
    };

    // -------------------------------------------------------------------------
    // The persistent state
    // -------------------------------------------------------------------------

    // One groom entity's LOD memory, advanced once per frame.
    //
    // FOUR STABILITY COUNTERS, NOT ONE, and that is criterion 3 showing up in
    // the state rather than only in the curves: a shadow step that keeps
    // flickering must not hold the visibility step at its old value, or the
    // three budgets are coupled again through their hysteresis even though
    // their curves are separate.
    struct GroomLodState
    {
        // 4-byte members first, the two one-byte enums and their explicit
        // padding last: operator== is a whole-object memcmp (issue #1019).

        /// What the policy asked for last frame, and for how many consecutive
        /// frames it has asked for it.
        u32 RepresentationStableFrames = 0;

        u32 VisibilityStep = 0;
        u32 RequestedVisibilityStep = 0;
        u32 VisibilityStableFrames = 0;

        u32 SimulationStep = 0;
        u32 RequestedSimulationStep = 0;
        u32 SimulationStableFrames = 0;

        u32 ShadowStep = 0;
        u32 RequestedShadowStep = 0;
        u32 ShadowStableFrames = 0;

        /// What the groom is drawn as right now.
        GroomRepresentation Representation = GroomRepresentation::Strand;
        GroomRepresentation RequestedRepresentation = GroomRepresentation::Strand;
        u8 Pad0 = 0;
        u8 Pad1 = 0;

        [[nodiscard]] auto operator==(const GroomLodState& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomLodState) == 44,
                  "GroomLodState is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomLodState>);

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    struct GroomLodDecision
    {
        // 4-byte members first, the one-byte enum and its explicit padding
        // last: operator== is a whole-object memcmp (issue #1019).

        GroomLodFallbackReason Reason = GroomLodFallbackReason::NotRequested;

        /// The step each axis runs at, after hysteresis.
        u32 VisibilityStep = 0;
        u32 SimulationStep = 0;
        u32 ShadowStep = 0;

        /// 2^-step for the two axes that spend a COUNT. The shadow axis spends
        /// a resolution and its step is added to CoatLodPolicy's, so it has no
        /// fraction of its own and deliberately does not get one here.
        f32 VisibilityFraction = 1.0f;
        f32 SimulationFraction = 1.0f;

        /// The apparent size the decision was made at, carried so a panel can
        /// show the input beside the answer.
        f32 PixelSize = 0.0f;

        GroomRepresentation Representation = GroomRepresentation::Strand;

        /// True on the ONE frame the representation actually changed.
        ///
        /// Carried on the decision rather than recomputed by each consumer,
        /// because the only thing that knows is the state this call just
        /// advanced — and a consumer comparing "what I have now" against "what
        /// I had last frame" would be maintaining a second copy of the
        /// hysteresis state, which is how two copies drift. The renderer's
        /// counter sums it, and a counter that stays at the groom count IS
        /// thrashing, reported before anyone has to see it.
        bool RepresentationChanged = false;
        u8 Pad0 = 0;
        u8 Pad1 = 0;

        /// True when the coat is NOT on the tier its apparent size selects.
        /// NotRequested is excluded for GroomCompositionDecision's reason: a
        /// scene full of grooms that never opted in is not a scene full of
        /// failures.
        [[nodiscard]] constexpr bool IsFallback() const noexcept
        {
            return Reason != GroomLodFallbackReason::None && Reason != GroomLodFallbackReason::NotRequested;
        }

        [[nodiscard]] auto operator==(const GroomLodDecision& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomLodDecision) == 32,
                  "GroomLodDecision is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomLodDecision>);

    /// The identity decision: what a groom with no LOD policy gets. Every
    /// strand, every guide, the shadow policy's own step, forever.
    [[nodiscard]] GroomLodDecision IdentityGroomLodDecision() noexcept;

    /// Advances `state` by one frame and returns this frame's decision.
    ///
    /// PURE GIVEN (policy, inputs, state): it reads no clock, no camera and no
    /// global. `state` is in/out because the hysteresis IS the state — passing
    /// it by value and returning a copy would let a caller advance the stability
    /// counters twice in a frame, which is exactly how a two-viewport scene
    /// would halve the hold this relies on.
    ///
    /// The policy is sanitised on the way in, so a caller that reached this
    /// with raw authored numbers still cannot make the ladder non-monotone.
    GroomLodDecision AdvanceGroomLod(const GroomLodPolicy& policy, const GroomLodInputs& inputs,
                                     GroomLodState& state) noexcept;

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    // What a user reads when a coat does not look the way they set it up to.
    // Per REPRESENTATION as well as per reason, because criterion 4 asks for
    // cost and memory reported BY representation and a count is half of that.
    struct GroomLodStats
    {
        u32 GroomsConsidered = 0;
        u32 GroomsOnSelectedTier = 0;
        u32 GroomsFellBack = 0;

        std::array<u32, GroomRepresentationCount> ByRepresentation{};
        std::array<u32, static_cast<sizet>(GroomLodFallbackReason::Count)> ByReason{};

        /// Strands drawn and GPU bytes resident, split by representation. The
        /// issue asks for cost and memory "by animal/group/representation"; the
        /// animal and group splits are per-entity readouts in the inspector,
        /// and this is the frame-wide one the renderer panel shows.
        std::array<u32, GroomRepresentationCount> StrandsByRepresentation{};
        std::array<u64, GroomRepresentationCount> BytesByRepresentation{};

        /// Representation changes that actually happened this frame. A steady
        /// camera scores 0; a camera crossing a threshold scores 1 and then 0
        /// for at least HoldFrames frames. A number that stays at the groom
        /// count IS thrashing, reported before anyone has to see it.
        u32 RepresentationChanges = 0;

        /// Worst and mean width compensation actually applied, and how many
        /// grooms hit the cap. A coat at the cap is genuinely thinner than it
        /// was authored, and criterion 1 is a claim about exactly that.
        f32 MaxWidthCompensation = 1.0f;
        u32 GroomsAtCompensationCap = 0;

        void Record(const GroomLodDecision& decision) noexcept
        {
            ++GroomsConsidered;
            if (decision.IsFallback())
            {
                ++GroomsFellBack;
            }
            else
            {
                ++GroomsOnSelectedTier;
            }
            ByReason[static_cast<sizet>(decision.Reason)] += 1u;
            ByRepresentation[static_cast<sizet>(decision.Representation)] += 1u;
            if (decision.RepresentationChanged)
            {
                ++RepresentationChanges;
            }
        }

        void Reset() noexcept
        {
            *this = GroomLodStats{};
        }

        /// The reason to show when only one line fits. Skips None and
        /// NotRequested — neither is something to fix — and breaks ties toward
        /// the more fundamental (lower) reason, so the answer names a cause
        /// rather than a symptom.
        [[nodiscard]] GroomLodFallbackReason DominantFallbackReason() const noexcept
        {
            auto dominant = GroomLodFallbackReason::None;
            u32 best = 0;
            for (sizet i = static_cast<sizet>(GroomLodFallbackReason::NotRequested) + 1u; i < ByReason.size(); ++i)
            {
                if (ByReason[i] > best)
                {
                    best = ByReason[i];
                    dominant = static_cast<GroomLodFallbackReason>(i);
                }
            }
            return dominant;
        }

        /// Field by field rather than a whole-object memcmp: this struct holds
        /// std::arrays of two different widths, so its layout has a hole a
        /// memcmp would read — and the one float in it needs
        /// Math::BitwiseEqual rather than a float `==` either way.
        [[nodiscard]] auto operator==(const GroomLodStats& other) const -> bool
        {
            return GroomsConsidered == other.GroomsConsidered &&
                   GroomsOnSelectedTier == other.GroomsOnSelectedTier &&
                   GroomsFellBack == other.GroomsFellBack && ByRepresentation == other.ByRepresentation &&
                   ByReason == other.ByReason && StrandsByRepresentation == other.StrandsByRepresentation &&
                   BytesByRepresentation == other.BytesByRepresentation &&
                   RepresentationChanges == other.RepresentationChanges &&
                   Math::BitwiseEqual(MaxWidthCompensation, other.MaxWidthCompensation) &&
                   GroomsAtCompensationCap == other.GroomsAtCompensationCap;
        }
    };
} // namespace OloEngine
