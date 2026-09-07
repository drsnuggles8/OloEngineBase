#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>

namespace OloEngine
{
    // =========================================================================
    // The reflection hierarchy's shared vocabulary — issue #1057, ADR 0020.
    //
    // The contract in one line: every tier estimates the SAME quantity (the
    // radiance along the specular lobe) and reports a CONFIDENCE in [0,1] saying
    // how much of that lobe it is entitled to answer for. The tiers composite
    // bottom-up as an ordered "over", with the bottom tier pinned at confidence
    // 1, which makes the weights telescope to EXACTLY one.
    //
    // That identity is the whole no-double-count guarantee, and it is what
    // ReflectionTierContractTest asserts — for arbitrary confidence vectors, not
    // for a hand-picked scene.
    // =========================================================================

    // The tiers, ordered best-informed first. The numbering is the ORDER: a
    // tier composites over every tier with a LARGER value.
    enum class ReflectionTier : u32
    {
        Planar = 0,   ///< A true mirror of one plane. Correct where it applies, and only there.
        SSR = 1,      ///< Screen-space march. Cannot see off-screen or occluded content.
        RayQuery = 2, ///< #1057's new tier: exactly the off-screen / occluded gap SSR leaves.
        ProbeIBL = 3, ///< DDGI probes over the global prefilter. THE BOTTOM TIER — always confidence 1.

        Count
    };

    inline constexpr u32 kReflectionTierCount = static_cast<u32>(ReflectionTier::Count);

    [[nodiscard]] constexpr std::string_view ToString(ReflectionTier tier)
    {
        switch (tier)
        {
            case ReflectionTier::Planar:
                return "planar";
            case ReflectionTier::SSR:
                return "SSR";
            case ReflectionTier::RayQuery:
                return "ray query";
            case ReflectionTier::ProbeIBL:
                return "probe/IBL";
            case ReflectionTier::Count:
                break;
        }
        return "unknown";
    }

    // The per-pixel confidences, ordered by ReflectionTier. This is the input to
    // the contract; the effective weights are derived from it, never stored
    // alongside it, so the two cannot disagree.
    using ReflectionTierConfidences = std::array<f32, kReflectionTierCount>;

    // The effective weight of every tier, per ADR 0020 §1:
    //
    //     w_t = c_t * PRODUCT over u ABOVE t of (1 - c_u)
    //
    // with the bottom tier taking the whole remaining residual. Evaluating it
    // this way — rather than as four hand-written expressions — is why adding a
    // tier cannot introduce a double-count: there is one formula and it
    // telescopes.
    //
    // Every confidence is clamped to [0,1] on the way in. A confidence outside
    // that range is the one thing that could break the sum, so it is corrected
    // here rather than trusted; the shaders clamp too, and both are deliberate.
    [[nodiscard]] constexpr ReflectionTierConfidences ComputeReflectionTierWeights(
        const ReflectionTierConfidences& confidences)
    {
        ReflectionTierConfidences weights{};
        f32 residual = 1.0f; // the share no higher tier has claimed yet

        // Every tier except the bottom one claims c_t of what is left.
        for (u32 tier = 0; tier + 1u < kReflectionTierCount; ++tier)
        {
            // The predicate order is load-bearing, not style. `NaN < 0.0f` and
            // `NaN > 1.0f` are BOTH false, so the obvious spelling
            // (`c < 0 ? 0 : (c > 1 ? 1 : c)`) passes a NaN straight through and
            // poisons every weight below it. Testing `> 0.0f` FIRST sends NaN
            // down the else branch to 0 — "this tier answered nothing" — which
            // degrades to the tier below instead of letting a broken tier claim
            // the whole pixel. ReflectionTierContractTest pins both halves.
            const f32 raw = confidences[tier];
            const f32 confidence = (raw > 0.0f) ? ((raw > 1.0f) ? 1.0f : raw) : 0.0f;
            weights[tier] = residual * confidence;
            residual -= weights[tier];
        }

        // THE BOTTOM TIER TAKES THE WHOLE REMAINDER, whatever its own reported
        // confidence says. That is not a shortcut, it is the invariant the
        // guarantee rests on (ADR 0020 §7): a probe/IBL tier allowed to "admit
        // it doesn't know" would leave energy unclaimed and darken the frame.
        // Uncertainty at the bottom widens the lobe; it never lowers the weight.
        weights[kReflectionTierCount - 1u] = residual;
        return weights;
    }

    // Which tier actually owns the pixel — the largest effective weight. Ties go
    // to the higher tier, which is the one that claimed first.
    [[nodiscard]] constexpr ReflectionTier DominantReflectionTier(const ReflectionTierConfidences& confidences)
    {
        const auto weights = ComputeReflectionTierWeights(confidences);
        u32 best = 0;
        for (u32 tier = 1; tier < kReflectionTierCount; ++tier)
        {
            if (weights[tier] > weights[best])
                best = tier;
        }
        return static_cast<ReflectionTier>(best);
    }

    // Why the ray-query tier stood down. Reported once per change, never
    // silently — docs/agent-rules/no-silent-fallbacks.md.
    enum class ReflectionTierFallbackReason : u32
    {
        None = 0,                   ///< The ray-query tier is live.
        NotRequested,               ///< The setting is off. Not a failure; the normal case.
        RayTracingUnavailable,      ///< No RT device, wrong backend, or OLO_VULKAN_NO_RAY_TRACING=1.
        AccelerationStructureEmpty, ///< RT is up but no TLAS has been built — nothing to trace against.
        GPUSceneUnavailable,        ///< No instance / geometry / material tables, so a hit cannot be shaded.
        TargetUnavailable,          ///< The graph produced no output target this frame.
        ShaderUnavailable,          ///< The shader never loaded (the non-RT backend never creates it).

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ReflectionTierFallbackReason reason)
    {
        switch (reason)
        {
            case ReflectionTierFallbackReason::None:
                return "the ray-query reflection tier is active";
            case ReflectionTierFallbackReason::NotRequested:
                return "the ray-query reflection tier is switched off in the render settings";
            case ReflectionTierFallbackReason::RayTracingUnavailable:
                return "hardware ray tracing is unavailable on this device (see RayTracing::UnsupportedReason)";
            case ReflectionTierFallbackReason::AccelerationStructureEmpty:
                return "no TLAS has been built yet, so there is nothing to trace against";
            case ReflectionTierFallbackReason::GPUSceneUnavailable:
                return "the GPU Scene tables are unavailable, so a ray hit could not be shaded";
            case ReflectionTierFallbackReason::TargetUnavailable:
                return "the graph produced no ray-traced reflection target this frame";
            case ReflectionTierFallbackReason::ShaderUnavailable:
                return "the ray-query reflection shader is not loaded on this backend";
            case ReflectionTierFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // What the ray-query tier did this frame. Counted rather than commented,
    // because both of the first slice's quality limits are invisible in a still
    // frame and would otherwise be discovered by a reviewer instead of reported
    // by the engine.
    struct ReflectionTierStats
    {
        bool RayQueryTierActive = false;
        ReflectionTierFallbackReason Fallback = ReflectionTierFallbackReason::NotRequested;

        // An UPPER BOUND, derived rather than measured — the same honest form
        // ShadowTechniqueStats::ShadowRaysDispatchedUpperBound uses. Pixels that
        // early-out on the roughness gate or on sky depth dispatch no ray, and
        // the shader has no way to report back how many did.
        u64 ReflectionRaysDispatchedUpperBound = 0;

        // #805: hits are shaded from untextured material factors. Non-zero
        // whenever the tier ran at all, because every hit in this slice is
        // untextured — it is a standing limitation, not an occasional one.
        bool HitsShadedUntextured = false;
        // Masked geometry reflects as its full quad (no any-hit alpha test
        // without the sampler heap), the same trade RayTracedShadowPass makes.
        bool MaskedGeometryReflectsAsSolid = false;

        void Reset() noexcept
        {
            *this = ReflectionTierStats{};
        }
    };

    // @brief Artist / debug controls for the ray-query reflection tier (#1057).
    struct RayTracedReflectionSettings
    {
        bool Enabled = false;

        // Rays are spent only where the specular lobe is narrow enough for one
        // deterministic mirror ray to mean something. Above GateEnd the
        // confidence is exactly 0 and the probes answer, which is the right
        // trade: a ray budget buys nothing on a rough surface.
        f32 RoughnessGateStart = 0.05f;
        f32 RoughnessGateEnd = 0.30f;

        f32 MaxRayDistance = 60.0f;      ///< Metres before a ray is abandoned.
        f32 RayOriginNormalBias = 0.02f; ///< Metres along the normal, to escape the surface.
        f32 Intensity = 1.0f;            ///< Overall tier strength; folds into the confidence.

        // A second visibility ray per hit, so a reflected surface standing in
        // shadow is not lit as though it were in the open. Off is a bisect
        // lever and roughly halves the tier's ray count.
        bool TraceSunShadowRay = true;

        // The prefilter mip the hit's ambient is read from. High on purpose: it
        // stands in for irradiance, which this tier has no separate source for.
        f32 SkyAmbientLod = 4.0f;

        // "Which tier answered this pixel", drawn in place of the composite.
        // Part of the contract, not a nicety — a hierarchy whose selection
        // cannot be seen per pixel is one nobody can review (ADR 0020 §7).
        bool TierDebugView = false;

        // Required, not optional: PostProcessSettings holds this struct by value
        // and defaults its OWN operator==, so without one here that operator is
        // implicitly DELETED and the editor's undo-tracking snapshot compare
        // (PostProcessFullSnapshot) stops compiling. Same defaulted form the
        // other settings structs in PostProcessSettings.h use — the float
        // members are compared bitwise for CHANGE DETECTION only, never to ask
        // whether two settings are physically equivalent, which is the
        // distinction CLAUDE.md's no-`==`-on-floats rule is about.
        bool operator==(const RayTracedReflectionSettings&) const = default;
    };

} // namespace OloEngine
