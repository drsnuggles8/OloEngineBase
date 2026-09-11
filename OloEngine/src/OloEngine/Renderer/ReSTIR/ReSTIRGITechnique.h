#pragma once

// =============================================================================
// ReSTIRGITechnique.h — when the ReSTIR GI tier engages, why it stood down, who
// owns the indirect-diffuse term while it runs, and what it did. Issue #1169
// (#979 Phase 3, second half).
//
// WHY THIS FILE EXISTS. Two of #979's constraints are not suggestions:
//
//     "DDGI remains fallback/cache/lower tier."
//     "ReSTIR GI and DDGI must not double-count."
//
// The second is a non-goal with no natural home — it is a property of how three
// passes compose, so if it lives inside any one of them nobody can inspect it,
// test it without a GPU, or explain a frame that looks twice as bright as it
// should. So the composition is a PURE FUNCTION here
// (SelectIndirectDiffuseSources), in ShadowTechnique.h's shape and for the same
// reasons: constexpr, backend-neutral, and testable on the CI runners, which are
// also the machines that take the fallback path.
//
// THE HAND-OFF RULE, in one line (design note §5):
//
//     The probe cache is read at exactly ONE vertex per path. Enabling ReSTIR GI
//     MOVES which vertex that is; it does not add a second read.
//
//   ReSTIR GI off — the cache is read at x0, in ComputeDeferredLit's ambient
//                   ladder. The path is (eye, x0, cache).
//   ReSTIR GI on  — the cache is read at x1, inside the GI initial-sample draw,
//                   as the tail beyond the resampled bounce. At x0 the diffuse
//                   indirect is the resolved ReSTIR GI radiance. The path is
//                   (eye, x0, x1, cache).
//
// Neither configuration reads the cache twice on one path, which is why
// composing them is not double-counting and why DDGI stays exactly what #979
// calls it — the tier that supplies bounces 2 and beyond, which a one-bounce
// estimator cannot.
//
// WHAT THIS FILE DELIBERATELY CANNOT SEE. A wrong Jacobian, a mis-normalised MIS
// weight, a reservoir read at the wrong layout version, or a reconnection that
// leaked through a wall. Those are ReservoirGI.h's and the shaders', pinned by
// ReSTIRGIContractTest and by the oracle comparison — never by this header.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirGI.h"

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

    // What produced this frame's indirect DIFFUSE term at the primary shading
    // point. Exactly one of these owns it, always.
    //
    // ProbeCache is the whole existing ladder — baked lightmaps, baked SH probes
    // and realtime DDGI atlases, whichever the bound volume selects. #979 is
    // explicit that it is permanent, so this enum draws exactly one distinction:
    // whether the resampled one-bounce estimate replaced it AT THE PRIMARY
    // VERTEX.
    enum class IndirectDiffuseTechnique : u8
    {
        ProbeCache = 0, ///< Baked GI / probe volumes / DDGI, read at x0. Always available.
        ReSTIRGI,       ///< Resampled one-bounce diffuse at x0, with the cache moved to x1.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(IndirectDiffuseTechnique technique)
    {
        switch (technique)
        {
            case IndirectDiffuseTechnique::ProbeCache:
                return "probe cache (baked / DDGI)";
            case IndirectDiffuseTechnique::ReSTIRGI:
                return "ReSTIR GI";
            case IndirectDiffuseTechnique::Count:
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
    enum class ReSTIRGIFallbackReason : u32
    {
        None = 0,                   ///< The tier is live.
        NotRequested,               ///< The setting is off. Not a failure; the normal case.
        RenderingPathUnsupported,   ///< The reservoir passes are G-Buffer consumers; forward / Forward+ have none.
        ShaderUnavailable,          ///< The shaders never loaded (the non-RT backend never creates them).
        RayTracingUnavailable,      ///< No RT device, wrong backend, or OLO_VULKAN_NO_RAY_TRACING=1.
        AccelerationStructureEmpty, ///< RT is up but no TLAS has been built — no bounce ray can be traced.
        GPUSceneUnavailable,        ///< No instance / geometry / material / light tables, so no bounce can be shaded.
        TargetUnavailable,          ///< The graph produced no reservoir targets this frame.
        LayoutVersionMismatch,      ///< The history holds reservoirs of a different kGIReservoirLayoutVersion.
        // The MEASURED stand-down, and the only one that is a judgement rather
        // than a missing capability. See ReSTIRGIEngagePredicate for why it is
        // the ONLY one, and why DI's count comparison has no GI analogue.
        NoIndirectSourceInScene,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ReSTIRGIFallbackReason reason)
    {
        switch (reason)
        {
            case ReSTIRGIFallbackReason::None:
                return "ReSTIR GI is active";
            case ReSTIRGIFallbackReason::NotRequested:
                return "ReSTIR GI is switched off in the render settings";
            case ReSTIRGIFallbackReason::RenderingPathUnsupported:
                return "the reservoir passes need the deferred G-Buffer; this frame is forward / Forward+";
            case ReSTIRGIFallbackReason::ShaderUnavailable:
                return "the ReSTIR GI shaders are not loaded on this backend";
            case ReSTIRGIFallbackReason::RayTracingUnavailable:
                return "hardware ray tracing is unavailable on this device (see RayTracing::UnsupportedReason)";
            case ReSTIRGIFallbackReason::AccelerationStructureEmpty:
                return "no TLAS has been built yet, so no bounce ray could be traced";
            case ReSTIRGIFallbackReason::GPUSceneUnavailable:
                return "the GPU Scene tables are unavailable, so a bounce vertex could not be shaded";
            case ReSTIRGIFallbackReason::TargetUnavailable:
                return "the graph produced no reservoir targets this frame";
            case ReSTIRGIFallbackReason::LayoutVersionMismatch:
                return "the reservoir history was written by a different GI reservoir layout version";
            case ReSTIRGIFallbackReason::NoIndirectSourceInScene:
                return "the scene has no light, no emissive triangle and no environment, so one bounce has "
                       "nothing to gather and two rays per pixel would buy the black image the ambient "
                       "ladder already produces";
            case ReSTIRGIFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // The measured engagement criterion, and why there is only one
    // -------------------------------------------------------------------------

    // WHY DI'S CRITERION HAS NO GI ANALOGUE, stated here because the obvious move
    // is to copy it and it would be wrong (design note §9).
    //
    // ReSTIRDIEngagePredicate stands the DI tier down when the emitter set does
    // not exceed the per-pixel candidate budget, because below that RIS with
    // M >= N ENUMERATES THE SAME SET the clustered loop already walks, and pays
    // four passes to do it. That argument needs a cheaper mechanism computing the
    // SAME integral by enumeration.
    //
    // DDGI is not that. It is a COARSER CACHE of the same integral, at probe
    // resolution, with a different error characteristic — there is no scene size
    // at which the ReSTIR GI estimator degenerates into the DDGI one, so there is
    // no count to compare. Inventing one would be the rename #1169 warns against,
    // dressed as a policy.
    //
    // AND IT IS DELIBERATELY NOT A VARIANCE THRESHOLD, for the reason DI states:
    // a variance threshold reads last frame's noise, so it HUNTS — the tier
    // engages, the noise drops, the tier disengages, the noise returns, and the
    // image flickers between two estimators at a few Hz. Variance stays exposed
    // through the Variance debug view, as something to LOOK at rather than
    // something to switch on.
    //
    // What is left is one genuinely measured stand-down, and it is a property of
    // the SCENE, which changes when the scene changes and not when the estimator
    // does.
    struct ReSTIRGIEngageInputs
    {
        // Live lights of EVERY type, including directional. That differs from
        // ReSTIRDIEngageInputs::CandidateLightCount on purpose: DI excludes
        // directional lights because the clustered loop keeps them for their
        // cascades, their mask channel and their cloud shadow, whereas the sun
        // bouncing off a floor is exactly the indirect light THIS tier exists
        // for. Same word, different set — which is why this is its own type and
        // not DI's.
        u32 LightCount = 0;
        // Emissive triangles in the shared table.
        u32 EmissiveTriangles = 0;
        // Whether a sky / environment source exists for a bounce ray that
        // escapes. An outdoor scene with no lights at all is lit entirely by
        // this, so leaving it out would stand the tier down on exactly the
        // scenes it renders best.
        bool EnvironmentAvailable = false;

        [[nodiscard]] auto operator==(const ReSTIRGIEngageInputs&) const -> bool = default;
    };

    // Engage unless there is nothing for one bounce to gather.
    //
    // No margin and no hysteresis, unlike DI's: this is not a boundary a scene
    // can sit on and drift across one light at a time. Going from "no light at
    // all" to "one light" is an event, not a fluctuation.
    [[nodiscard]] constexpr bool ReSTIRGIEngagePredicate(const ReSTIRGIEngageInputs& inputs)
    {
        return inputs.LightCount != 0u || inputs.EmissiveTriangles != 0u || inputs.EnvironmentAvailable;
    }

    // -------------------------------------------------------------------------
    // The hand-off — the #979 non-goal, as a pure function
    // -------------------------------------------------------------------------

    // WHERE EACH ESTIMATE OF THE INDIRECT DIFFUSE TERM IS ALLOWED TO LAND. Four
    // booleans rather than one enum because they are not alternatives: two of
    // them describe the SAME cache read at DIFFERENT vertices, and the fourth is
    // a separate pass entirely. An enum would have hidden exactly the relation
    // the non-goal is about.
    struct IndirectDiffuseSources
    {
        // The probe ladder's irradiance added at the primary shading point, in
        // ComputeDeferredLit. The pre-#1169 behaviour.
        bool DDGIAtPrimary = true;
        // The resolved ReSTIR GI radiance added at the primary shading point.
        bool ReSTIRGIAtPrimary = false;
        // The probe ladder's irradiance read at the BOUNCE VERTEX, inside the GI
        // initial-sample draw, as the path tail beyond the resampled bounce.
        // This is what keeps DDGI a cache rather than switching it off: bounces
        // 2 and beyond still come from it.
        bool DDGIAtSecondary = false;
        // SSGI's post-hoc composite. A THIRD estimate of the same integral, so
        // it must stand down when ReSTIR GI owns the term or the frame
        // double-counts.
        bool SSGIComposite = false;

        [[nodiscard]] auto operator==(const IndirectDiffuseSources&) const -> bool = default;
    };

    struct IndirectDiffuseInputs
    {
        bool ReSTIRGIActive = false;
        // The user's SSGI switch. Honoured only while ReSTIR GI is standing
        // down; the stand-down it causes is counted, not silent.
        bool SSGIRequested = false;
        // The user's "let the bounce vertex read the probe cache" switch. Off
        // makes the tier strictly one bounce, which is a QUALITY choice (indoor
        // scenes lose their multi-bounce fill) and never a correctness one —
        // there is no configuration in which turning it on double-counts,
        // because x1 is not x0.
        bool DDGITailRequested = true;

        [[nodiscard]] auto operator==(const IndirectDiffuseInputs&) const -> bool = default;
    };

    // The whole non-goal, in one place.
    //
    // THE INVARIANT ReSTIRGIContractTest ASSERTS, for every combination of
    // inputs: DDGIAtPrimary and ReSTIRGIAtPrimary are never both true and never
    // both false — exactly one mechanism owns the term at the primary vertex —
    // and SSGIComposite is false whenever ReSTIRGIAtPrimary is.
    [[nodiscard]] constexpr IndirectDiffuseSources SelectIndirectDiffuseSources(
        const IndirectDiffuseInputs& inputs)
    {
        if (!inputs.ReSTIRGIActive)
        {
            return IndirectDiffuseSources{ .DDGIAtPrimary = true,
                                           .ReSTIRGIAtPrimary = false,
                                           // No secondary vertex exists, so there is nothing to read
                                           // the cache AT. Reporting true here would be a claim about
                                           // a path that was never traced.
                                           .DDGIAtSecondary = false,
                                           .SSGIComposite = inputs.SSGIRequested };
        }
        return IndirectDiffuseSources{ .DDGIAtPrimary = false,
                                       .ReSTIRGIAtPrimary = true,
                                       .DDGIAtSecondary = inputs.DDGITailRequested,
                                       // NOT inputs.SSGIRequested. This is the double count the
                                       // non-goal names, and it is the one that would look like a
                                       // slightly-too-bright room rather than like a bug.
                                       .SSGIComposite = false };
    }

    // -------------------------------------------------------------------------
    // Tuning
    // -------------------------------------------------------------------------

    // What the resolve pass can put on screen instead of the resampled indirect
    // radiance. Every one of these exists because #979's non-goal is explicit:
    // "do not use 'path traced' to mean one noisy sample plus an opaque
    // denoiser." A tier whose intermediate state cannot be looked at is exactly
    // that.
    enum class ReSTIRGIDebugView : u32
    {
        Radiance = 0,           ///< The resampled one-bounce indirect diffuse. The normal case.
        RawCandidate = 1,       ///< The initial sample with no reuse at all — the un-denoised signal.
        HistoryValidity = 2,    ///< The #976 validity verdict that gated temporal reuse.
        Variance = 3,           ///< Per-pixel variance of the resolved radiance.
        ReservoirM = 4,         ///< Confidence weight: how many candidates this pixel's sample stands for.
        ReservoirW = 5,         ///< The unbiased contribution weight.
        SampleKind = 6,         ///< Surface hit vs environment escape.
        SampleAge = 7,          ///< Frames since the surviving sample's vertex was traced. Lineage.
        SampleRadiance = 8,     ///< L_o at the bounce vertex, before the shift and the BRDF at x0.
        ReconnectionLength = 9, ///< Distance from this pixel to its sample vertex — where J is stressed.
        BiasClampState = 10,    ///< Where the Jacobian, the domain gate, the clamp or the ray intervened.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ReSTIRGIDebugView view)
    {
        switch (view)
        {
            case ReSTIRGIDebugView::Radiance:
                return "radiance";
            case ReSTIRGIDebugView::RawCandidate:
                return "raw candidate";
            case ReSTIRGIDebugView::HistoryValidity:
                return "history validity";
            case ReSTIRGIDebugView::Variance:
                return "variance";
            case ReSTIRGIDebugView::ReservoirM:
                return "reservoir M";
            case ReSTIRGIDebugView::ReservoirW:
                return "reservoir W";
            case ReSTIRGIDebugView::SampleKind:
                return "sample kind";
            case ReSTIRGIDebugView::SampleAge:
                return "sample age";
            case ReSTIRGIDebugView::SampleRadiance:
                return "sample radiance";
            case ReSTIRGIDebugView::ReconnectionLength:
                return "reconnection length";
            case ReSTIRGIDebugView::BiasClampState:
                return "bias / clamp state";
            case ReSTIRGIDebugView::Count:
                break;
        }
        return "unknown";
    }

    // The compile-time bounds the shaders' loops are written against. A setting
    // past one of these would silently do less than it was asked to, so the pass
    // clamps to them and COUNTS what the clamp cut off.
    //
    // THE INITIAL-CANDIDATE CEILING IS 8, NOT DI'S 64, and that is the economic
    // difference between the two tiers rather than a conservative guess: a DI
    // candidate is a table lookup and some arithmetic, so 32 of them cost one
    // ray. A GI candidate is a BOUNCE RAY plus an NEE SHADOW RAY at the vertex
    // it finds, so 32 of them cost 64 rays per pixel. The default is 1, which is
    // what the literature runs, and the ceiling exists for A/B work rather than
    // for shipping.
    inline constexpr u32 kReSTIRGIMaxInitialCandidates = 8u;
    inline constexpr u32 kReSTIRGIMaxSpatialNeighbours = 16u;
    inline constexpr f32 kReSTIRGIMaxTemporalMCap = 1024.0f;

    struct ReSTIRGISettings
    {
        bool Enabled = false;

        // Bounce rays drawn per pixel per frame, cosine-weighted over the
        // hemisphere. See kReSTIRGIMaxInitialCandidates for why the default is 1
        // and DI's is 32.
        u32 InitialCandidates = 1;

        // Read the probe cache at the BOUNCE VERTEX as the path tail — bounces 2
        // and beyond (design note §5). Off makes the tier strictly one bounce:
        // an indoor scene loses its multi-bounce fill and reads darker, which is
        // a quality choice, not a correctness one.
        bool DDGITail = true;

        bool TemporalReuse = true;
        // The cap on M. Bounds how much of a pixel's estimate a stale sample can
        // still claim.
        f32 TemporalMCap = ReSTIR::kDefaultTemporalMCap;
        // The cap on a sample's AGE in frames — the second, independent
        // staleness bound (design note §10). #976's validity test only looks at
        // the RECEIVING surface, so a sample whose own vertex was re-lit or moved
        // passes it every time; this is what bounds that.
        u32 MaxSampleAge = ReSTIR::kDefaultMaxSampleAge;

        bool SpatialReuse = true;
        u32 SpatialNeighbours = 4;
        // Neighbour search radius in pixels. Wider than DI's 16 because the
        // indirect signal is smoother and tolerates it, and because a GI pixel
        // has far fewer candidates to start with.
        f32 SpatialRadiusPixels = 24.0f;
        u32 SpatialPasses = 1;

        ReSTIR::BiasMode BiasMode = ReSTIR::BiasMode::UnbiasedMIS;

        // The reconnection visibility ray in the RESOLVE, on the surviving
        // sample only (design note §6.2). Without it, reuse lights surfaces
        // through walls — and it does so SMOOTHLY, because reuse is spatially
        // coherent, which is the shape nobody files. Off is for isolating the
        // resampling from the ray cost when profiling, not for shipping.
        bool ReconnectionVisibility = true;
        // The same ray, per spatial NEIGHBOUR. Costs k rays per pixel and buys
        // the removal of the residual leak the default arm has. Off by default,
        // and the fact that the default arm HAS a residual leak is stated rather
        // than left to be discovered.
        bool SpatialReconnectionVisibility = false;

        // Below this distance a reconnection is rejected rather than scaled
        // (design note §6.3). No DI analogue.
        f32 MinReconnectionDistance = ReSTIR::kDefaultMinimumReconnectionDistance;

        // Bounce rays terminate here, metres. A finite value is a quality knob,
        // not a correctness one: what it drops is long-range indirect, which is
        // exactly what the probe cache at the vertex is good at. Large by
        // default so an unconfigured scene is not quietly darkened.
        f32 MaxBounceDistance = 1.0e4f;

        // Firefly clamp on the final resampled radiance. A clamp is a BIAS, so
        // it is off by default and any run that claims to match the oracle must
        // leave it off. <= 0 disables it.
        f32 MaxRadianceClamp = 0.0f;

        // Ray offset along the geometric normal, metres. Same reason as
        // RayTracedShadowSettings::RayOriginNormalBias: the G-Buffer position is
        // reconstructed from a quantised depth.
        f32 RayOriginNormalBias = 0.02f;

        ReSTIRGIDebugView DebugView = ReSTIRGIDebugView::Radiance;

        // Required for the same reason GpuPathTracerSettings states: the settings
        // struct that holds this one by value defaults its own operator==, which
        // is implicitly deleted without one here. The floats are compared bitwise
        // for CHANGE DETECTION only, never to ask whether two settings are
        // physically equivalent.
        [[nodiscard]] auto operator==(const ReSTIRGISettings&) const -> bool = default;
    };

    // Every knob held to its range, in ONE place, applied on scene load and again
    // before every upload — values also arrive from a live edit, a script or an
    // MCP write, and the shader cannot be the backstop.
    [[nodiscard]] inline ReSTIRGISettings SanitizeReSTIRGISettings(const ReSTIRGISettings& in) noexcept
    {
        const ReSTIRGISettings defaults{};
        const auto finiteOr = [](f32 value, f32 fallback) noexcept
        { return std::isfinite(value) ? value : fallback; };

        ReSTIRGISettings s = in;
        s.InitialCandidates = std::clamp(s.InitialCandidates, 1u, kReSTIRGIMaxInitialCandidates);
        s.SpatialNeighbours = std::min(s.SpatialNeighbours, kReSTIRGIMaxSpatialNeighbours);
        s.SpatialPasses = std::clamp(s.SpatialPasses, 1u, 4u);
        s.TemporalMCap =
            std::clamp(finiteOr(s.TemporalMCap, defaults.TemporalMCap), 1.0f, kReSTIRGIMaxTemporalMCap);
        // 1, never 0: an age cap of zero would drop every sample on the frame
        // after it was traced, which is temporal reuse switched off by another
        // name — and it would report itself as running.
        s.MaxSampleAge = std::clamp(s.MaxSampleAge, 1u, ReSTIR::kMaxSampleAgeFrames);
        s.SpatialRadiusPixels =
            std::clamp(finiteOr(s.SpatialRadiusPixels, defaults.SpatialRadiusPixels), 1.0f, 128.0f);
        s.MinReconnectionDistance =
            std::clamp(finiteOr(s.MinReconnectionDistance, defaults.MinReconnectionDistance), 0.0f, 10.0f);
        s.MaxBounceDistance =
            std::clamp(finiteOr(s.MaxBounceDistance, defaults.MaxBounceDistance), 0.1f, 1.0e6f);
        s.MaxRadianceClamp =
            std::clamp(finiteOr(s.MaxRadianceClamp, defaults.MaxRadianceClamp), 0.0f, 1.0e6f);
        s.RayOriginNormalBias =
            std::clamp(finiteOr(s.RayOriginNormalBias, defaults.RayOriginNormalBias), 0.0f, 1.0f);
        if (std::to_underlying(s.BiasMode) >= std::to_underlying(ReSTIR::BiasMode::Count))
            s.BiasMode = defaults.BiasMode;
        if (std::to_underlying(s.DebugView) >= std::to_underlying(ReSTIRGIDebugView::Count))
            s.DebugView = ReSTIRGIDebugView::Radiance;
        return s;
    }

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    // Everything the choice depends on, gathered so the function is pure. The
    // caller assembles it from what the frame ACTUALLY resolved — never from what
    // it expects to resolve, which is how a first frame ends up sampling a
    // reservoir target that does not exist.
    struct ReSTIRGITechniqueInputs
    {
        bool Requested = false;
        bool DeferredPathActive = false;  ///< A G-Buffer exists this frame.
        bool ShadersReady = false;        ///< All four reservoir shaders loaded.
        bool RayTracingAvailable = false; ///< RayTracingScene::IsAvailable().
        bool TlasReady = false;           ///< GetTlasDeviceAddress() != 0.
        bool GPUSceneAvailable = false;   ///< Instance / geometry / material / light tables are addressable.
        bool TargetsAvailable = false;    ///< The graph produced this frame's reservoir targets.
        bool HistoryLayoutMatches = true; ///< The history planes were written at kGIReservoirLayoutVersion.

        ReSTIRGIEngageInputs Engagement{};

        [[nodiscard]] auto operator==(const ReSTIRGITechniqueInputs&) const -> bool = default;
    };

    struct ReSTIRGITechniqueDecision
    {
        IndirectDiffuseTechnique Effective = IndirectDiffuseTechnique::ProbeCache;
        ReSTIRGIFallbackReason Reason = ReSTIRGIFallbackReason::NotRequested;

        [[nodiscard]] constexpr bool IsReSTIR() const
        {
            return Effective == IndirectDiffuseTechnique::ReSTIRGI;
        }

        [[nodiscard]] auto operator==(const ReSTIRGITechniqueDecision&) const -> bool = default;
    };

    [[nodiscard]] constexpr ReSTIRGITechniqueDecision SelectReSTIRGITechnique(
        const ReSTIRGITechniqueInputs& inputs)
    {
        const auto fallback = [](ReSTIRGIFallbackReason reason)
        {
            return ReSTIRGITechniqueDecision{ .Effective = IndirectDiffuseTechnique::ProbeCache,
                                              .Reason = reason };
        };

        if (!inputs.Requested)
            return fallback(ReSTIRGIFallbackReason::NotRequested);
        if (!inputs.DeferredPathActive)
            return fallback(ReSTIRGIFallbackReason::RenderingPathUnsupported);
        if (!inputs.ShadersReady)
            return fallback(ReSTIRGIFallbackReason::ShaderUnavailable);
        if (!inputs.RayTracingAvailable)
            return fallback(ReSTIRGIFallbackReason::RayTracingUnavailable);
        if (!inputs.TlasReady)
            return fallback(ReSTIRGIFallbackReason::AccelerationStructureEmpty);
        if (!inputs.GPUSceneAvailable)
            return fallback(ReSTIRGIFallbackReason::GPUSceneUnavailable);
        if (!inputs.TargetsAvailable)
            return fallback(ReSTIRGIFallbackReason::TargetUnavailable);
        if (!inputs.HistoryLayoutMatches)
            return fallback(ReSTIRGIFallbackReason::LayoutVersionMismatch);
        // LAST, and deliberately so: every guard above names a missing capability
        // the user can act on, and reporting "this scene has nothing to bounce"
        // in front of "this device has no ray tracing" would bury the reason that
        // actually explains the frame.
        if (!ReSTIRGIEngagePredicate(inputs.Engagement))
            return fallback(ReSTIRGIFallbackReason::NoIndirectSourceInScene);

        return ReSTIRGITechniqueDecision{ .Effective = IndirectDiffuseTechnique::ReSTIRGI,
                                          .Reason = ReSTIRGIFallbackReason::None };
    }

    // -------------------------------------------------------------------------
    // The counters
    // -------------------------------------------------------------------------

    // What the tier did this frame. Counted rather than commented, because the
    // standing limits are invisible in a still frame and would otherwise be
    // discovered by a reviewer instead of reported by the engine.
    struct ReSTIRGIStats
    {
        bool Active = false;
        ReSTIRGIFallbackReason Fallback = ReSTIRGIFallbackReason::NotRequested;
        std::array<u32, static_cast<sizet>(ReSTIRGIFallbackReason::Count)> ByReason{};

        // Which normalisation produced this frame. Reported rather than assumed
        // from the settings, because the pass clamps an out-of-range value.
        ReSTIR::BiasMode BiasMode = ReSTIR::BiasMode::UnbiasedMIS;
        u32 ReservoirLayoutVersion = ReSTIR::kGIReservoirLayoutVersion;

        // The hand-off, as the frame actually composed it. Reported rather than
        // derived by a reader from Active, because the SSGI stand-down and the
        // DDGI tail are separate switches and "why is this room darker than it
        // was" is answerable from here alone.
        IndirectDiffuseSources Sources{};
        // SSGI was requested and this tier stood it down. A count rather than a
        // bool so a user who cannot find their SSGI slider's effect has a number
        // to look at (docs/agent-rules/no-silent-fallbacks.md).
        u32 SSGIStoodDown = 0;

        ReSTIRGIEngageInputs Engagement{};

        // What actually ran.
        u32 InitialCandidatesPerPixel = 0;
        u32 SpatialNeighboursPerPixel = 0;
        u32 SpatialPasses = 0;
        bool TemporalReuseRan = false;
        bool ReconnectionVisibilityRan = false;
        bool SpatialReconnectionVisibilityRan = false;
        bool DDGITailRan = false;
        u32 MaxSampleAge = 0;

        // How many of the FIVE history planes the registry actually handed back
        // this frame. Temporal reuse needs all five, so a value of 0-4 is the
        // difference between "temporal reuse is off" and "temporal reuse was
        // asked for and could not run" — two states that look identical in the
        // image (both are just noisier) and that a single bool cannot separate.
        u32 HistoryPlanesAvailable = 0;
        static constexpr u32 kHistoryPlaneCount = 5;

        // An UPPER BOUND, derived rather than measured — the same honest form the
        // shadow, reflection and path-tracing tiers use. Per pixel and per
        // candidate: one bounce ray plus one NEE shadow ray at the vertex it
        // finds; plus the resolve's one reconnection ray; plus one per spatial
        // neighbour per pass when the per-neighbour test is on. Sky pixels and
        // pixels whose reservoir is empty trace none, and the shader cannot
        // report how many did.
        u64 RaysDispatchedUpperBound = 0;

        // Settings values the pass clamped before upload. Non-zero means the
        // frame did LESS than the settings asked for.
        u32 SettingsClamped = 0;

        void Reset()
        {
            *this = ReSTIRGIStats{};
        }

        void Record(const ReSTIRGITechniqueDecision& decision)
        {
            ByReason[static_cast<sizet>(decision.Reason)]++;
            Active = decision.IsReSTIR();
            Fallback = decision.Reason;
        }

        [[nodiscard]] auto operator==(const ReSTIRGIStats&) const -> bool = default;
    };

} // namespace OloEngine
