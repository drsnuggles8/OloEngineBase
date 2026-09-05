#pragma once

// =============================================================================
// ShadowTechnique.h — which mechanism answers "is this point in shadow?", and
// why it is not the one that was asked for. Issue #1056.
//
// WHY THIS FILE EXISTS. Until now the engine did not CHOOSE a shadow
// technique: include/DeferredLightingShared.glsl carried a literal two-way
// GLSL branch (if (VSM_ENABLED != 0) ... else ...), and every other shadow
// decision was a bool on ShadowSettings. Adding ray-traced shadows as a third
// hard-coded if would have made the branch a four-way one whose arms are
// selected by three unrelated flags — and would have left the interesting
// question, "this light asked for ray tracing and did not get it, why?",
// unanswerable from anywhere.
//
// SO THE SELECTION IS A VALUE, NOT A BRANCH. SelectShadowTechnique is a pure
// function from "what the settings asked for" plus "what this frame can
// actually do" to "what this light gets, and the reason it is not what was
// asked for". It is backend-neutral and constexpr, so it compiles and is
// tested on a machine with no GPU — which is every CI runner this project has,
// and which is also the machine that takes the FALLBACK path, so the fallback
// is the arm CI covers best. That is deliberate: the house rule is that a path
// which cannot do its job says so loudly and countably, and a fallback nobody
// tests is the exact shape of a silently unshadowed light.
//
// WHAT IT DELIBERATELY DOES NOT COVER. It cannot see a wrong ray offset, a
// stale acceleration structure, or a mask sampled at the wrong channel. Those
// are device facts, pinned by the device-backed RayTracedShadowPass tests and
// by the visual-evidence captures — never by this header.
//
// WHY NOT A RenderingPath::HybridRT. RenderingPath.h reserved a commented-out
// HybridRT row for this, and promoting it was the other option the issue
// offered. It is the wrong seam: shadow technique is orthogonal to the
// G-Buffer strategy, so folding it into the path enum multiplies paths
// combinatorially (Deferred x {shadow map, VSM, ray traced} x a future
// reflections tier) and, more concretely, would silently change the meaning of
// the ~70 `Path == RenderingPath::Deferred` predicates already in the tree.
// Virtual Shadow Maps made the same call for the same reason and live on
// ShadowSettings. The reserved row now records that decision instead.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The technique
    // -------------------------------------------------------------------------

    // What produces the visibility term for one light.
    //
    // ShadowMap is the whole existing raster tier — CSM cascades, the local
    // light atlas, Virtual Shadow Maps, PCF and PCSS. Those are selected BELOW
    // this enum by flags that already exist on ShadowSettings; the distinction
    // this enum draws is the one that changes which PASS runs and which
    // texture the lighting shader reads. #979 is explicit that the raster tier
    // is permanent, not throwaway code behind a migration.
    enum class ShadowTechnique : u8
    {
        ShadowMap = 0, ///< Projected depth: CSM / atlas / VSM. Always available.
        RayTraced,     ///< A ray-query visibility mask. Vulkan + RT device only.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ShadowTechnique technique)
    {
        switch (technique)
        {
            case ShadowTechnique::ShadowMap:
                return "ShadowMap";
            case ShadowTechnique::RayTraced:
                return "RayTraced";
            case ShadowTechnique::Count:
                break;
        }
        return "Unknown";
    }

    // -------------------------------------------------------------------------
    // Why it is not what was asked for
    // -------------------------------------------------------------------------

    // Ordered most-fundamental first, in RayTracing::UnsupportedReason's style:
    // a light that trips an earlier row would trip later ones too, so the
    // FIRST match is reported and the counter is unambiguous. Each reason
    // string is a sentence a human can act on, not a token — these surface in
    // the log and in the renderer statistics panel.
    enum class ShadowTechniqueFallbackReason : u32
    {
        None = 0,                   ///< No fallback: the light got what it asked for.
        NotRequested,               ///< ShadowSettings::Technique is ShadowMap. Not a failure; the normal case.
        RenderingPathUnsupported,   ///< The mask is a G-Buffer consumer; forward / Forward+ have no G-Buffer.
        RayTracingUnavailable,      ///< No RT device, wrong backend, or OLO_VULKAN_NO_RAY_TRACING=1.
        AccelerationStructureEmpty, ///< RT is up but no TLAS has been built — nothing to trace against.
        MaskUnavailable,            ///< The graph produced no shadow mask this frame (disabled / no target).
        MaskChannelBudgetExhausted, ///< More lights opted in than the mask has channels.
        LightNotShadowCasting,      ///< The light casts no shadow at all; neither technique runs.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(ShadowTechniqueFallbackReason reason)
    {
        switch (reason)
        {
            case ShadowTechniqueFallbackReason::None:
                return "ray-traced shadows are active for this light";
            case ShadowTechniqueFallbackReason::NotRequested:
                return "shadow-map shadows were requested (ShadowSettings::Technique)";
            case ShadowTechniqueFallbackReason::RenderingPathUnsupported:
                return "the ray-traced shadow mask needs the deferred G-Buffer; this frame is forward / Forward+";
            case ShadowTechniqueFallbackReason::RayTracingUnavailable:
                return "hardware ray tracing is unavailable on this device (see RayTracing::UnsupportedReason)";
            case ShadowTechniqueFallbackReason::AccelerationStructureEmpty:
                return "no TLAS has been built yet, so there is nothing to trace against";
            case ShadowTechniqueFallbackReason::MaskUnavailable:
                return "the ray-traced shadow mask was not produced this frame";
            case ShadowTechniqueFallbackReason::MaskChannelBudgetExhausted:
                return "more lights opted into ray-traced shadows than the mask has channels";
            case ShadowTechniqueFallbackReason::LightNotShadowCasting:
                return "this light casts no shadow at all";
            case ShadowTechniqueFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // The mask budget
    // -------------------------------------------------------------------------

    // One RGBA16F screen-space target carries FOUR lights' visibility, one per
    // channel. That number is the target's channel count, not a tuning knob: a
    // fifth ray-traced light needs a second target, and the honest answer to
    // asking for one is MaskChannelBudgetExhausted rather than a silently
    // unshadowed light or a quietly reused channel.
    inline constexpr u32 kRayTracedShadowMaskChannels = 4;

    // Sentinel for "this light has no mask channel". Named rather than a bare
    // -1 so a call site comparing against it reads as the question it is.
    inline constexpr i32 kNoRayTracedShadowChannel = -1;

    // -------------------------------------------------------------------------
    // Tuning
    // -------------------------------------------------------------------------

    // The knobs of the ray-traced tier. Kept apart from ShadowSettings' raster
    // fields because none of them mean anything to a shadow map, and folding
    // them in would make "which of these does the active technique read?" a
    // question the struct cannot answer.
    struct RayTracedShadowSettings
    {
        // Shadow rays per pixel per light. One is the design point: the
        // temporal accumulation plus the variance-guided spatial filter are
        // what turn one stochastic sample into a smooth penumbra, and raising
        // this multiplies the trace cost linearly for a signal the denoiser
        // already reaches. Clamped to [1, 8].
        u32 RaysPerPixel = 1;

        // The light's angular radius, in DEGREES of half-angle, as seen from
        // the receiver. THIS is what makes the penumbra a geometric fact
        // rather than a filter width: the ray is jittered inside the cone this
        // subtends, so an occluder far from the receiver spreads the cone's
        // footprint and the shadow softens with distance on its own. The sun
        // is ~0.265 degrees; larger values read as an overcast sky.
        f32 LightAngularRadiusDegrees = 0.5f;

        // How far a shadow ray travels before it gives up, in metres. This is
        // NOT ShadowSettings::MaxShadowDistance — that one bounds the CASCADE
        // coverage, and the whole point here is that an occluder beyond it
        // still shadows. It exists only so an unbounded ray does not traverse
        // a whole open world for a light that is metres away; 0 means "no
        // limit", which is correct for a directional light in a closed scene.
        f32 MaxRayDistance = 0.0f;

        // World-space offset along the geometric normal before tracing, in
        // metres. The ray-tracing equivalent of the shadow-map normal bias,
        // and needed for the same reason: the G-Buffer position is
        // reconstructed from a quantised depth, so a ray started exactly on
        // the surface self-intersects. Too large detaches contact shadows.
        f32 RayOriginNormalBias = 0.02f;

        // Temporal accumulation of the visibility signal. Off makes the
        // stochastic estimate visible as per-pixel noise — useful when
        // judging the raw trace, wrong for shipping.
        bool TemporalAccumulation = true;
        f32 TemporalFeedback = 0.90f;
        f32 TemporalClipGamma = 1.5f;

        // Variance-guided spatial filter after the temporal resolve. Its
        // radius is driven by the accumulated variance, so a converged region
        // is left alone and a disoccluded one is blurred hard.
        bool SpatialFilter = true;
        // Maximum filter radius in pixels at full variance. Clamped to [0, 8].
        f32 SpatialFilterRadius = 4.0f;

        [[nodiscard]] auto operator==(const RayTracedShadowSettings&) const -> bool = default;
    };

    // One light that asked for a ray-traced shadow, as the frame's light setup
    // saw it. Published by Scene's shadow setup, which is the only place that
    // holds both the light's component and its index in the multi-light UBO;
    // RayTracedShadowPass decides whether the request can actually be honoured,
    // because it is the only place that knows whether the trace ran.
    //
    // It lives in this header rather than beside the pass so the producer
    // (Scene), the transport (Renderer3D) and the consumer (the pass) share one
    // definition without any of them pulling in the render-graph headers.
    struct RayTracedShadowLightRequest
    {
        // Index into UBOStructures::MultiLightUBO::Lights. This is the number
        // the lighting shader matches against ShadowUBO's channel routing, so
        // it is the identity that has to survive the trip.
        i32 UboLightIndex = -1;
        bool Directional = false;
        // World direction TOWARD the light (directional) or world position
        // (punctual). Which one it is comes from Directional, never from
        // inspecting the vector.
        glm::vec3 Vector{ 0.0f, 1.0f, 0.0f };
        // Angular half-radius in DEGREES for a directional light; emitter
        // radius in METRES for a punctual one. Two units in one field is
        // deliberate: they describe the same thing (how big the source looks
        // from here) and only the shader can convert the second, because only
        // it has the receiver distance.
        f32 Shape = 0.0f;
        f32 Range = 0.0f; ///< Punctual falloff range, metres. 0 = unbounded.

        [[nodiscard]] auto operator==(const RayTracedShadowLightRequest&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The space the rays are traced in
    // -------------------------------------------------------------------------

    // THE ONE THING ABOUT THIS FEATURE A TEST CANNOT SEE BY LOOKING AT PIXELS.
    //
    // The acceleration structure is built from GPU Scene's RENDER-RELATIVE
    // instance transforms (issue #429), and every shader's reconstructed "world
    // position" is render-relative too, because the camera UBO is made relative
    // before upload. A ray traced from an ABSOLUTE world position is therefore
    // displaced from the geometry it is tracing against by exactly the render
    // origin.
    //
    // That displacement is ZERO near the world origin — which is where every
    // test scene and every benchmark scene in this repo sits, so the bug is
    // invisible to a rendered frame, a golden image and a device test alike. It
    // becomes a 1024 m miss the moment the origin grid snaps. So the conversion
    // lives here, as two pure functions, and is pinned by arithmetic rather than
    // by a picture.
    //
    // A DIRECTION IS NOT A POSITION, and this is the whole trap: a directional
    // light's vector is translation-invariant and must NOT be shifted, while a
    // punctual light's is a point and must be. Shifting both, or neither, is a
    // plausible-looking image that is wrong.

    // View matrix mapping RENDER-RELATIVE world positions to view space, from
    // the world view matrix and the origin GPU Scene encoded against.
    [[nodiscard]] inline glm::mat4 MakeRayTracedShadowView(const glm::mat4& worldView,
                                                           const glm::vec3& renderOrigin)
    {
        // Same construction as CameraRelative.h's MakeViewRelative; spelled out
        // rather than included so this header stays free of renderer headers and
        // the test can reach it. RayTracedShadowSpaceTest pins the two equal.
        glm::mat4 relative = worldView;
        relative[3] = worldView * glm::vec4(renderOrigin, 1.0f);
        return relative;
    }

    // The light vector as the trace shader must receive it.
    [[nodiscard]] inline glm::vec3 MakeRayTracedShadowLightVector(const RayTracedShadowLightRequest& request,
                                                                  const glm::vec3& renderOrigin)
    {
        return request.Directional ? request.Vector : (request.Vector - renderOrigin);
    }

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    // Everything the choice depends on, gathered so the function is pure. The
    // caller assembles it from the settings and from what the frame ACTUALLY
    // resolved — never from what it expects to resolve, which is how a first
    // frame ends up sampling a mask that does not exist.
    struct ShadowTechniqueInputs
    {
        ShadowTechnique Requested = ShadowTechnique::ShadowMap;

        bool LightCastsShadows = true;    ///< The light's own shadow toggle.
        bool DeferredPathActive = false;  ///< A G-Buffer exists this frame.
        bool RayTracingAvailable = false; ///< RayTracingScene::IsAvailable().
        bool TlasReady = false;           ///< GetTlasDeviceAddress() != 0.
        bool MaskAvailable = false;       ///< The graph produced the mask target this frame.
    };

    struct ShadowTechniqueDecision
    {
        ShadowTechnique Effective = ShadowTechnique::ShadowMap;
        ShadowTechniqueFallbackReason Reason = ShadowTechniqueFallbackReason::NotRequested;
        // Which mask channel this light reads, or kNoRayTracedShadowChannel.
        // Only ever set when Effective == RayTraced.
        i32 MaskChannel = kNoRayTracedShadowChannel;

        [[nodiscard]] constexpr bool IsRayTraced() const
        {
            return Effective == ShadowTechnique::RayTraced;
        }

        [[nodiscard]] auto operator==(const ShadowTechniqueDecision&) const -> bool = default;
    };

    // `channelsAlreadyAssigned` is how many mask channels earlier lights in
    // this frame have taken. Passing it in rather than keeping a counter
    // inside is what keeps the function pure and its budget behaviour testable
    // without a frame.
    [[nodiscard]] constexpr ShadowTechniqueDecision SelectShadowTechnique(const ShadowTechniqueInputs& inputs,
                                                                          u32 channelsAlreadyAssigned)
    {
        const auto fallback = [](ShadowTechniqueFallbackReason reason)
        {
            return ShadowTechniqueDecision{ .Effective = ShadowTechnique::ShadowMap,
                                            .Reason = reason,
                                            .MaskChannel = kNoRayTracedShadowChannel };
        };

        // A light that casts nothing is reported first and separately: neither
        // technique runs for it, so counting it as a ray-tracing fallback
        // would inflate the fallback total with lights that were never going
        // to trace a ray.
        if (!inputs.LightCastsShadows)
            return fallback(ShadowTechniqueFallbackReason::LightNotShadowCasting);

        if (inputs.Requested != ShadowTechnique::RayTraced)
            return fallback(ShadowTechniqueFallbackReason::NotRequested);

        if (!inputs.DeferredPathActive)
            return fallback(ShadowTechniqueFallbackReason::RenderingPathUnsupported);
        if (!inputs.RayTracingAvailable)
            return fallback(ShadowTechniqueFallbackReason::RayTracingUnavailable);
        if (!inputs.TlasReady)
            return fallback(ShadowTechniqueFallbackReason::AccelerationStructureEmpty);
        if (!inputs.MaskAvailable)
            return fallback(ShadowTechniqueFallbackReason::MaskUnavailable);
        if (channelsAlreadyAssigned >= kRayTracedShadowMaskChannels)
            return fallback(ShadowTechniqueFallbackReason::MaskChannelBudgetExhausted);

        return ShadowTechniqueDecision{ .Effective = ShadowTechnique::RayTraced,
                                        .Reason = ShadowTechniqueFallbackReason::None,
                                        .MaskChannel = static_cast<i32>(channelsAlreadyAssigned) };
    }

    // -------------------------------------------------------------------------
    // The counters
    // -------------------------------------------------------------------------

    // Per-frame, reset where the decisions are made. A zero in RayTracedLights
    // is ambiguous on its own — off, unsupported, or no shadowed lights in the
    // scene — which is exactly why ByReason exists beside it and why the
    // reasons are enumerated rather than logged.
    struct ShadowTechniqueStats
    {
        u32 RayTracedLights = 0; ///< Lights whose visibility came from a ray query.
        u32 FallbackLights = 0;  ///< Lights that asked for ray tracing and did not get it.
        // Every light that was considered, by reason. NotRequested and
        // LightNotShadowCasting are counted here too but NOT in FallbackLights:
        // neither is a failure to deliver something that was asked for.
        std::array<u32, static_cast<sizet>(ShadowTechniqueFallbackReason::Count)> ByReason{};

        // TLAS instances whose geometry the acceleration structure classified
        // as masked (alpha tested). They shadow as SOLID here, because a ray
        // query has no per-draw scope to bind an arbitrary material's alpha
        // map and the shader-visible sampler heap that would fix it is issue
        // #805. Counted rather than merely commented, so the artefact is
        // diagnosable when someone asks why a leaf casts a rectangle.
        u32 MaskedOccludersShadowedAsSolid = 0;

        // Shadow rays the trace dispatched this frame, as an UPPER BOUND:
        // width * height * raysPerPixel * rayTracedLights. It is an upper bound
        // and not a measurement because the shader early-outs on sky pixels and
        // on surfaces facing away from the light, and counting the survivors
        // would need a GPU atomic and a readback. Stated as a bound rather than
        // published as a count, because a number whose relationship to reality
        // is unclear is worse than one that is honestly labelled.
        u64 ShadowRaysDispatchedUpperBound = 0;

        void Record(const ShadowTechniqueDecision& decision)
        {
            ByReason[static_cast<sizet>(decision.Reason)]++;
            if (decision.IsRayTraced())
            {
                RayTracedLights++;
                return;
            }
            if (decision.Reason != ShadowTechniqueFallbackReason::NotRequested &&
                decision.Reason != ShadowTechniqueFallbackReason::LightNotShadowCasting)
            {
                FallbackLights++;
            }
        }

        void Reset()
        {
            *this = ShadowTechniqueStats{};
        }

        // The single reason to report when one has to be picked — the most
        // common genuine failure. Used by the log line and the statistics
        // panel, which have room for a sentence, not a histogram.
        [[nodiscard]] ShadowTechniqueFallbackReason DominantFallbackReason() const
        {
            auto dominant = ShadowTechniqueFallbackReason::None;
            u32 best = 0;
            for (sizet i = 0; i < ByReason.size(); ++i)
            {
                const auto reason = static_cast<ShadowTechniqueFallbackReason>(i);
                if (reason == ShadowTechniqueFallbackReason::None ||
                    reason == ShadowTechniqueFallbackReason::NotRequested ||
                    reason == ShadowTechniqueFallbackReason::LightNotShadowCasting)
                {
                    continue;
                }
                if (ByReason[i] > best)
                {
                    best = ByReason[i];
                    dominant = reason;
                }
            }
            return dominant;
        }

        [[nodiscard]] auto operator==(const ShadowTechniqueStats&) const -> bool = default;
    };
} // namespace OloEngine
