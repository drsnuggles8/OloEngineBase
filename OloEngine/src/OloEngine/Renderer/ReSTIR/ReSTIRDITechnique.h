#pragma once

// =============================================================================
// ReSTIRDITechnique.h — when the ReSTIR DI tier engages, why it stood down, and
// what it did. Issue #1140 (#979 Phase 3).
//
// WHY THIS FILE EXISTS. #979's constraint on this tier is not a suggestion:
//
//     "ReSTIR DI is enabled only when candidate count/variance justifies its
//      overhead. DDGI remains fallback/cache/lower tier."
//
// That makes the engage/stand-down decision a MEASURED one, and a measured
// decision that lives inside a pass is a decision nobody can inspect, test
// without a GPU, or explain to a user staring at a frame that looks like the
// tier is off. So it is a pure function here, in ShadowTechnique.h's shape and
// for the same reasons: constexpr, backend-neutral, and testable on the CI
// runners, which are also the machines that take the fallback path.
//
// THE FALLBACK IS NEVER SILENT. Standing down means the frame is lit by
// clustered lighting plus shadow maps — which is a correct image, just a
// lower tier — and the ONE thing that must not happen is that nobody can tell
// which of the two produced it. Every reason is enumerated, counted, and
// reported once per change (docs/agent-rules/no-silent-fallbacks.md).
//
// WHAT THIS FILE DELIBERATELY CANNOT SEE. It cannot see a wrong Jacobian, a
// mis-normalised MIS weight or a reservoir read at the wrong layout version.
// Those are ReservoirDI.h's, pinned by ReSTIRDIContractTest and by the oracle
// comparison against #1055's path tracer — never by this header.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirDI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The tier
    // -------------------------------------------------------------------------

    // What produced this frame's direct lighting.
    //
    // Clustered is the whole existing raster tier — clustered / Forward+ light
    // culling plus shadow maps or the ray-traced shadow mask. #979 is explicit
    // that it is permanent, not throwaway code behind a migration, so this enum
    // draws exactly one distinction: whether the reservoir passes ran.
    enum class DirectLightingTechnique : u8
    {
        Clustered = 0, ///< Clustered lighting + the shadow tier. Always available.
        ReSTIRDI,      ///< Screen-space reservoir resampling. Vulkan + RT device only.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(DirectLightingTechnique technique)
    {
        switch (technique)
        {
            case DirectLightingTechnique::Clustered:
                return "clustered";
            case DirectLightingTechnique::ReSTIRDI:
                return "ReSTIR DI";
            case DirectLightingTechnique::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // Why it is not what was asked for
    // -------------------------------------------------------------------------

    // Ordered most-fundamental first, so the FIRST match is reported and the
    // counter is unambiguous: a frame that trips an earlier row would trip the
    // later ones too. Each string is a sentence a human can act on.
    enum class ReSTIRDIFallbackReason : u32
    {
        None = 0,                   ///< The tier is live.
        NotRequested,               ///< The setting is off. Not a failure; the normal case.
        RenderingPathUnsupported,   ///< The reservoir passes are G-Buffer consumers; forward / Forward+ have none.
        ShaderUnavailable,          ///< The shaders never loaded (the non-RT backend never creates them).
        RayTracingUnavailable,      ///< No RT device, wrong backend, or OLO_VULKAN_NO_RAY_TRACING=1.
        AccelerationStructureEmpty, ///< RT is up but no TLAS has been built — no visibility ray can be traced.
        GPUSceneUnavailable,        ///< No instance / geometry / material / light tables, so no candidate can be shaded.
        TargetUnavailable,          ///< The graph produced no reservoir targets this frame.
        LayoutVersionMismatch,      ///< The history holds reservoirs of a different kReservoirLayoutVersion.
        // The MEASURED stand-down, and the only one that is a judgement rather
        // than a missing capability: the emitter set does not exceed the
        // per-pixel candidate budget, so resampling would enumerate the same set
        // the clustered loop already walks. See ReSTIRDIEngagePredicate.
        BelowEngagementThreshold,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ReSTIRDIFallbackReason reason)
    {
        switch (reason)
        {
            case ReSTIRDIFallbackReason::None:
                return "ReSTIR DI is active";
            case ReSTIRDIFallbackReason::NotRequested:
                return "ReSTIR DI is switched off in the render settings";
            case ReSTIRDIFallbackReason::RenderingPathUnsupported:
                return "the reservoir passes need the deferred G-Buffer; this frame is forward / Forward+";
            case ReSTIRDIFallbackReason::ShaderUnavailable:
                return "the ReSTIR DI shaders are not loaded on this backend";
            case ReSTIRDIFallbackReason::RayTracingUnavailable:
                return "hardware ray tracing is unavailable on this device (see RayTracing::UnsupportedReason)";
            case ReSTIRDIFallbackReason::AccelerationStructureEmpty:
                return "no TLAS has been built yet, so no visibility ray could be traced";
            case ReSTIRDIFallbackReason::GPUSceneUnavailable:
                return "the GPU Scene tables are unavailable, so a light candidate could not be shaded";
            case ReSTIRDIFallbackReason::TargetUnavailable:
                return "the graph produced no reservoir targets this frame";
            case ReSTIRDIFallbackReason::LayoutVersionMismatch:
                return "the reservoir history was written by a different reservoir layout version";
            case ReSTIRDIFallbackReason::BelowEngagementThreshold:
                return "the scene has no more emitters than this tier samples per pixel, so resampling "
                       "would enumerate the same set the clustered loop already walks, and cost four passes "
                       "to do it";
            case ReSTIRDIFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // The measured engagement criterion
    // -------------------------------------------------------------------------

    // What the criterion is allowed to look at. Both are MEASUREMENTS taken from
    // the frame that is about to be drawn, never predictions.
    //
    // WHY IT COMPARES AGAINST THE CANDIDATE BUDGET AND NOT AGAINST WHAT
    // CLUSTERED LIGHTING SAMPLES. Clustered lighting's per-pixel cost grows with
    // the light count; this tier's is FIXED at the candidate budget. So
    // resampling is buying something exactly when the emitter set is larger than
    // that budget — below it, RIS with M >= N enumerates the whole set anyway and
    // the clustered loop does the same work without four extra passes and five
    // scene-band RGBA32F targets.
    //
    // Comparing against "what clustered lighting samples" instead is the version
    // that looks right and is not: the multi-light array holds up to
    // MultiLightUBO::MAX_LIGHTS (256) lights and clustered lighting shades every
    // one of them, so a 200-light scene would compare 200 against 200 and never
    // engage — while 200 lights per pixel is precisely the cost this tier exists
    // to remove.
    //
    // The predicate is deliberately a COUNT comparison and not a variance
    // threshold, and that is worth stating plainly: a variance threshold reads
    // last frame's noise, so it HUNTS — the tier engages, the noise drops, the
    // tier disengages, the noise returns, and the image flickers between two
    // estimators at a few Hz. An emitter count is a property of the SCENE, which
    // changes when the scene changes and not when the estimator does. Variance is
    // still exposed, through the resolve's Variance debug view, as something to
    // LOOK at rather than something to switch on.
    struct ReSTIRDIEngageInputs
    {
        // Emitters this tier could resample over: live point / spot / sphere-area
        // lights plus emissive triangles. Directional lights are NOT counted —
        // the clustered loop keeps them so they keep their cascades, their mask
        // channel and their cloud shadow.
        u32 CandidateLightCount = 0;
        // RIS candidates the frame will draw per pixel — the tier's fixed cost,
        // and the count the emitter set has to exceed for resampling to be doing
        // anything an enumeration would not.
        u32 CandidateBudget = 0;

        [[nodiscard]] auto operator==(const ReSTIRDIEngageInputs&) const -> bool = default;
    };

    // Engage when the emitter set exceeds the per-pixel candidate budget by at
    // least the configured margin.
    //
    // The margin is the hysteresis: without it a scene sitting exactly on the
    // boundary toggles the tier as one light drifts in and out of the set, and
    // toggling between two estimators is more visible than either one's noise. It
    // is a tunable because the right value depends on how expensive the pass
    // chain is on the device, which this header cannot know.
    [[nodiscard]] constexpr bool ReSTIRDIEngagePredicate(const ReSTIRDIEngageInputs& inputs, u32 margin)
    {
        return inputs.CandidateLightCount > inputs.CandidateBudget &&
               (inputs.CandidateLightCount - inputs.CandidateBudget) >= margin;
    }

    // -------------------------------------------------------------------------
    // Tuning
    // -------------------------------------------------------------------------

    // What the resolve pass can put on screen instead of the resampled
    // radiance. Every one of these exists because #979's non-goal is explicit:
    // "do not use 'path traced' to mean one noisy sample plus an opaque
    // denoiser." A tier whose intermediate state cannot be looked at is exactly
    // that.
    enum class ReSTIRDIDebugView : u32
    {
        Radiance = 0,      ///< The resampled direct lighting. The normal case.
        RawCandidate = 1,  ///< The initial RIS sample with no reuse at all — the un-denoised signal.
        HistoryValidity = 2, ///< The #976 validity verdict that gated temporal reuse.
        Variance = 3,      ///< Per-pixel variance of the resolved radiance.
        ReservoirM = 4,    ///< Confidence weight: how many candidates this pixel's sample stands for.
        ReservoirW = 5,    ///< The unbiased contribution weight.
        SampleKind = 6,    ///< Which light family the surviving sample came from.
        BiasClampState = 7, ///< Where the Jacobian or the firefly clamp intervened.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ReSTIRDIDebugView view)
    {
        switch (view)
        {
            case ReSTIRDIDebugView::Radiance:
                return "radiance";
            case ReSTIRDIDebugView::RawCandidate:
                return "raw candidate";
            case ReSTIRDIDebugView::HistoryValidity:
                return "history validity";
            case ReSTIRDIDebugView::Variance:
                return "variance";
            case ReSTIRDIDebugView::ReservoirM:
                return "reservoir M";
            case ReSTIRDIDebugView::ReservoirW:
                return "reservoir W";
            case ReSTIRDIDebugView::SampleKind:
                return "sample kind";
            case ReSTIRDIDebugView::BiasClampState:
                return "bias / clamp state";
            case ReSTIRDIDebugView::Count:
                break;
        }
        return "unknown";
    }


    // The compile-time bounds the shaders' loops are written against. A setting
    // past one of these would silently do less than it was asked to, so the
    // pass clamps to them and COUNTS what the clamp cut off.
    inline constexpr u32 kReSTIRDIMaxInitialCandidates = 64u;
    inline constexpr u32 kReSTIRDIMaxSpatialNeighbours = 16u;
    inline constexpr f32 kReSTIRDIMaxTemporalMCap = 1024.0f;

    struct ReSTIRDISettings
    {
        bool Enabled = false;

        // RIS candidates drawn per pixel per frame from the light set. This is
        // the knob the tier exists for: it is what lets a thousand-light scene
        // cost the same as a thirty-two-light one. Clamped to
        // kReSTIRDIMaxInitialCandidates.
        u32 InitialCandidates = 32;

        // One visibility ray against the #978 TLAS for the surviving initial
        // candidate, before any reuse. Without it a shadowed sample propagates
        // through temporal and spatial reuse and lights a region that should be
        // dark — the classic ReSTIR light leak. Off is for isolating the
        // resampling from the ray cost when profiling, not for shipping.
        bool VisibilityReuse = true;

        bool TemporalReuse = true;
        // The cap on M. See ReservoirDI.h: it bounds how long a stale sample
        // survives a change in the scene.
        f32 TemporalMCap = ReSTIR::kDefaultTemporalMCap;

        bool SpatialReuse = true;
        u32 SpatialNeighbours = 4;
        // Neighbour search radius in pixels. Large radii find more independent
        // samples and put more strain on the Jacobian, which is exactly why the
        // bias mode is selectable.
        f32 SpatialRadiusPixels = 16.0f;
        // Spatial reuse passes, run in sequence. Two passes at a small radius
        // reach further than one at a large radius and keep the Jacobian
        // better conditioned.
        u32 SpatialPasses = 1;

        ReSTIR::BiasMode BiasMode = ReSTIR::BiasMode::UnbiasedMIS;

        // The margin in ReSTIRDIEngagePredicate.
        u32 EngagementMargin = 8;

        // Firefly clamp on the final resampled radiance. A clamp is a BIAS, so
        // it is off by default and any run that claims to match the oracle must
        // leave it off. <= 0 disables it.
        f32 MaxRadianceClamp = 0.0f;

        // Ray offset along the geometric normal for the visibility ray, metres.
        // Same reason as RayTracedShadowSettings::RayOriginNormalBias: the
        // G-Buffer position is reconstructed from a quantised depth.
        f32 RayOriginNormalBias = 0.02f;

        // What lands in the colour the rest of the frame consumes.
        ReSTIRDIDebugView DebugView = ReSTIRDIDebugView::Radiance;

        // Required for the same reason GpuPathTracerSettings states: the
        // settings struct that holds this one by value defaults its own
        // operator==, which is implicitly deleted without one here. The floats
        // are compared bitwise for CHANGE DETECTION only, never to ask whether
        // two settings are physically equivalent.
        [[nodiscard]] auto operator==(const ReSTIRDISettings&) const -> bool = default;
    };

    // Every knob held to its range, in ONE place, applied on scene load and
    // again before every upload — values also arrive from a live edit, a script
    // or an MCP write, and the shader cannot be the backstop.
    [[nodiscard]] inline ReSTIRDISettings SanitizeReSTIRDISettings(const ReSTIRDISettings& in) noexcept
    {
        const ReSTIRDISettings defaults{};
        const auto finiteOr = [](f32 value, f32 fallback) noexcept
        { return std::isfinite(value) ? value : fallback; };

        ReSTIRDISettings s = in;
        s.InitialCandidates = std::clamp(s.InitialCandidates, 1u, kReSTIRDIMaxInitialCandidates);
        s.SpatialNeighbours = std::min(s.SpatialNeighbours, kReSTIRDIMaxSpatialNeighbours);
        s.SpatialPasses = std::clamp(s.SpatialPasses, 1u, 4u);
        s.TemporalMCap = std::clamp(finiteOr(s.TemporalMCap, defaults.TemporalMCap), 1.0f,
                                    kReSTIRDIMaxTemporalMCap);
        s.SpatialRadiusPixels =
            std::clamp(finiteOr(s.SpatialRadiusPixels, defaults.SpatialRadiusPixels), 1.0f, 128.0f);
        s.MaxRadianceClamp =
            std::clamp(finiteOr(s.MaxRadianceClamp, defaults.MaxRadianceClamp), 0.0f, 1.0e6f);
        s.RayOriginNormalBias =
            std::clamp(finiteOr(s.RayOriginNormalBias, defaults.RayOriginNormalBias), 0.0f, 1.0f);
        s.EngagementMargin = std::min(s.EngagementMargin, 4096u);
        if (std::to_underlying(s.BiasMode) >= std::to_underlying(ReSTIR::BiasMode::Count))
            s.BiasMode = defaults.BiasMode;
        if (std::to_underlying(s.DebugView) >= std::to_underlying(ReSTIRDIDebugView::Count))
            s.DebugView = ReSTIRDIDebugView::Radiance;
        return s;
    }

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    // Everything the choice depends on, gathered so the function is pure. The
    // caller assembles it from what the frame ACTUALLY resolved — never from
    // what it expects to resolve, which is how a first frame ends up sampling a
    // reservoir target that does not exist.
    struct ReSTIRDITechniqueInputs
    {
        bool Requested = false;
        bool DeferredPathActive = false;  ///< A G-Buffer exists this frame.
        bool ShadersReady = false;        ///< All four reservoir shaders loaded.
        bool RayTracingAvailable = false; ///< RayTracingScene::IsAvailable().
        bool TlasReady = false;           ///< GetTlasDeviceAddress() != 0.
        bool GPUSceneAvailable = false;   ///< Instance / geometry / material / light tables are addressable.
        bool TargetsAvailable = false;    ///< The graph produced this frame's reservoir targets.
        bool HistoryLayoutMatches = true; ///< The history planes were written at kReservoirLayoutVersion.

        ReSTIRDIEngageInputs Engagement{};
        u32 EngagementMargin = 8;

        [[nodiscard]] auto operator==(const ReSTIRDITechniqueInputs&) const -> bool = default;
    };

    struct ReSTIRDITechniqueDecision
    {
        DirectLightingTechnique Effective = DirectLightingTechnique::Clustered;
        ReSTIRDIFallbackReason Reason = ReSTIRDIFallbackReason::NotRequested;

        [[nodiscard]] constexpr bool IsReSTIR() const
        {
            return Effective == DirectLightingTechnique::ReSTIRDI;
        }

        [[nodiscard]] auto operator==(const ReSTIRDITechniqueDecision&) const -> bool = default;
    };

    [[nodiscard]] constexpr ReSTIRDITechniqueDecision SelectReSTIRDITechnique(
        const ReSTIRDITechniqueInputs& inputs)
    {
        const auto fallback = [](ReSTIRDIFallbackReason reason)
        {
            return ReSTIRDITechniqueDecision{ .Effective = DirectLightingTechnique::Clustered,
                                              .Reason = reason };
        };

        if (!inputs.Requested)
            return fallback(ReSTIRDIFallbackReason::NotRequested);
        if (!inputs.DeferredPathActive)
            return fallback(ReSTIRDIFallbackReason::RenderingPathUnsupported);
        if (!inputs.ShadersReady)
            return fallback(ReSTIRDIFallbackReason::ShaderUnavailable);
        if (!inputs.RayTracingAvailable)
            return fallback(ReSTIRDIFallbackReason::RayTracingUnavailable);
        if (!inputs.TlasReady)
            return fallback(ReSTIRDIFallbackReason::AccelerationStructureEmpty);
        if (!inputs.GPUSceneAvailable)
            return fallback(ReSTIRDIFallbackReason::GPUSceneUnavailable);
        if (!inputs.TargetsAvailable)
            return fallback(ReSTIRDIFallbackReason::TargetUnavailable);
        if (!inputs.HistoryLayoutMatches)
            return fallback(ReSTIRDIFallbackReason::LayoutVersionMismatch);
        // LAST, and deliberately so: every guard above names a missing
        // capability the user can act on, and reporting "the scene has too few
        // lights" in front of "this device has no ray tracing" would bury the
        // reason that actually explains the frame.
        if (!ReSTIRDIEngagePredicate(inputs.Engagement, inputs.EngagementMargin))
            return fallback(ReSTIRDIFallbackReason::BelowEngagementThreshold);

        return ReSTIRDITechniqueDecision{ .Effective = DirectLightingTechnique::ReSTIRDI,
                                          .Reason = ReSTIRDIFallbackReason::None };
    }

    // -------------------------------------------------------------------------
    // The counters
    // -------------------------------------------------------------------------

    // What the tier did this frame. Counted rather than commented, because the
    // standing limits are invisible in a still frame and would otherwise be
    // discovered by a reviewer instead of reported by the engine.
    struct ReSTIRDIStats
    {
        bool Active = false;
        ReSTIRDIFallbackReason Fallback = ReSTIRDIFallbackReason::NotRequested;
        std::array<u32, static_cast<sizet>(ReSTIRDIFallbackReason::Count)> ByReason{};

        // Which normalisation produced this frame. Reported rather than assumed
        // from the settings, because the pass clamps an out-of-range value.
        ReSTIR::BiasMode BiasMode = ReSTIR::BiasMode::UnbiasedMIS;
        u32 ReservoirLayoutVersion = ReSTIR::kReservoirLayoutVersion;

        // The scene as the tier saw it, and the criterion's own inputs, so
        // "why did it stand down" is answerable from the statistics alone.
        ReSTIRDIEngageInputs Engagement{};
        u32 PunctualLights = 0;
        u32 SphereAreaLights = 0;
        u32 EmissiveTriangles = 0;

        // What actually ran.
        u32 InitialCandidatesPerPixel = 0;
        u32 SpatialNeighboursPerPixel = 0;
        u32 SpatialPasses = 0;
        bool TemporalReuseRan = false;
        bool VisibilityReuseRan = false;
        // How many of the FIVE history planes the registry actually handed back
        // this frame. Temporal reuse needs all five, so a value of 0-4 is the
        // difference between "temporal reuse is off" and "temporal reuse was
        // asked for and could not run" — two states that look identical in the
        // image (both are just noisier) and that a single bool cannot separate.
        u32 HistoryPlanesAvailable = 0;
        static constexpr u32 kHistoryPlaneCount = 5;

        // An UPPER BOUND, derived rather than measured — the same honest form
        // the shadow, reflection and path-tracing tiers use. Per pixel: one
        // visibility ray for the initial sample, one for the temporal
        // candidate, and one per spatial neighbour per pass. Pixels on the sky
        // and pixels whose reservoir is empty trace none, and the shader cannot
        // report how many did.
        u64 RaysDispatchedUpperBound = 0;

        // Live emitters at slots the shaders' loops cannot reach. They light
        // the clustered frame and the path tracer but not this tier.
        u32 LightsBeyondShaderBound = 0;
        // Settings values the pass clamped before upload. Non-zero means the
        // frame did LESS than the settings asked for.
        u32 SettingsClamped = 0;

        void Reset()
        {
            *this = ReSTIRDIStats{};
        }

        void Record(const ReSTIRDITechniqueDecision& decision)
        {
            ByReason[static_cast<sizet>(decision.Reason)]++;
            Active = decision.IsReSTIR();
            Fallback = decision.Reason;
        }

        [[nodiscard]] auto operator==(const ReSTIRDIStats&) const -> bool = default;
    };

} // namespace OloEngine
