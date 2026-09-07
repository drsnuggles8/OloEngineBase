#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace OloEngine
{
    // =========================================================================
    // The GPU reference path tracer's shared vocabulary — issue #1055 (#979
    // Phase 2). Settings the scene carries, the stats the pass reports, and the
    // reasons it can stand down. The pass itself is Passes/GpuPathTracerPass;
    // the shader is GpuPathTracer.glsl; the CPU oracle it mirrors is
    // Renderer/PathTracing/PathTracer.h.
    // =========================================================================

    // What the pass writes into the colour the post chain consumes. Radiance
    // is the tracer; the rest are the AOVs shown in place of it so the raw
    // signal, its variance and the accumulation state stay INSPECTABLE rather
    // than hidden behind a denoiser (#979's non-goal). Mirrored by the
    // OLO_PT_VIEW_* macros in GpuPathTracer.glsl — append, never renumber, the
    // value is serialized.
    enum class GpuPathTracerDebugView : u32
    {
        Radiance = 0,
        Albedo = 1,      ///< Mean first-hit albedo
        Normal = 2,      ///< Mean first-hit shading normal, remapped to [0,1]
        Variance = 3,    ///< Variance of the per-pixel mean, scaled for display
        SampleCount = 4, ///< Accumulated samples, scaled for display

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GpuPathTracerDebugView view)
    {
        switch (view)
        {
            case GpuPathTracerDebugView::Radiance:
                return "radiance";
            case GpuPathTracerDebugView::Albedo:
                return "albedo";
            case GpuPathTracerDebugView::Normal:
                return "normal";
            case GpuPathTracerDebugView::Variance:
                return "variance";
            case GpuPathTracerDebugView::SampleCount:
                return "sample count";
            case GpuPathTracerDebugView::Count:
                break;
        }
        return "unknown";
    }

    // Why the tracer stood down. Reported once per change, never silently —
    // docs/agent-rules/no-silent-fallbacks.md. Ordered most fundamental first,
    // the order GpuPathTracerPass resolves them in, so the reported reason is
    // the root cause rather than the first symptom.
    enum class GpuPathTracerFallbackReason : u32
    {
        None = 0,                   ///< The tracer is live.
        NotRequested,               ///< The setting is off. Not a failure; the normal case.
        ShaderUnavailable,          ///< The shader never loaded (the non-RT backend never creates it).
        RayTracingUnavailable,      ///< No RT device, wrong backend, or OLO_VULKAN_NO_RAY_TRACING=1.
        AccelerationStructureEmpty, ///< RT is up but no TLAS has been built — nothing to trace against.
        GPUSceneUnavailable,        ///< No instance / geometry / material tables, so a hit cannot be shaded.
        EmissiveTableNotGathered,   ///< The setting flipped on mid-frame, after the area-light gather was decided.
        TargetUnavailable,          ///< The graph produced no output target this frame.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GpuPathTracerFallbackReason reason)
    {
        switch (reason)
        {
            case GpuPathTracerFallbackReason::None:
                return "the GPU path tracer is active";
            case GpuPathTracerFallbackReason::NotRequested:
                return "the GPU path tracer is switched off in the render settings";
            case GpuPathTracerFallbackReason::ShaderUnavailable:
                return "the GPU path tracer shader is not loaded on this backend";
            case GpuPathTracerFallbackReason::RayTracingUnavailable:
                return "hardware ray tracing is unavailable on this device (see RayTracing::UnsupportedReason)";
            case GpuPathTracerFallbackReason::AccelerationStructureEmpty:
                return "no TLAS has been built yet, so there is nothing to trace against";
            case GpuPathTracerFallbackReason::GPUSceneUnavailable:
                return "the GPU Scene tables are unavailable, so a ray hit could not be shaded";
            case GpuPathTracerFallbackReason::EmissiveTableNotGathered:
                return "the area-light table was not gathered this frame (the tracer was switched on mid-frame); one "
                       "frame skipped";
            case GpuPathTracerFallbackReason::TargetUnavailable:
                return "the graph produced no path tracer target this frame";
            case GpuPathTracerFallbackReason::Count:
                break;
        }
        return "unknown";
    }

    // What the tracer did this frame. Counted rather than commented, because
    // the standing limits (untextured hits, masked geometry traced as solid,
    // Legacy materials shaded as ClosureV2, sphere-area lights ignored) are
    // invisible in a still frame and would otherwise be discovered by a
    // reviewer instead of reported by the engine.
    struct GpuPathTracerStats
    {
        bool Active = false;
        GpuPathTracerFallbackReason Fallback = GpuPathTracerFallbackReason::NotRequested;

        // The accumulation state the pass read this frame: the sample count
        // per pixel BEFORE this frame's samples were added (0 after an
        // invalidation), and whether a history was imported at all.
        bool HistoryValid = false;
        u32 AccumulatedSamplesPerPixel = 0;
        // What this frame added. Zero when the pixel cap (MaxSamples) is
        // reached — the frame is then a pass-through of the converged image.
        u32 SamplesTracedThisFrame = 0;

        // An UPPER BOUND, derived rather than measured — the same honest form
        // the shadow and reflection tiers use. Every sample traces at most
        // MaxBounces closest-hit rays plus, per bounce, one shadow ray per
        // punctual light and one for the emissive sample; a path that escapes
        // or dies early traces fewer, and the shader cannot report how many.
        u64 RaysDispatchedUpperBound = 0;

        // The scene as the tracer saw it.
        u32 EmissiveTriangles = 0;
        u32 PunctualLights = 0;
        u32 SphereAreaLightsIgnored = 0; ///< No reference twin, so neither oracle sees them.
        u32 LegacyMaterialsShadedAsClosureV2 = 0;
        f32 EmissiveTotalArea = 0.0f;

        // #805: hits are shaded from untextured material factors. True whenever
        // the tracer ran at all — a standing limitation, not an occasional one.
        bool HitsShadedUntextured = false;
        bool MaskedGeometryTracedAsSolid = false;

        // How many frames in a row the accumulation restarted from zero. One
        // is a camera move or an edit; a count that keeps climbing means
        // something invalidates EVERY frame (an animated entity, a camera
        // that never holds still) and the image can never converge — a state
        // that would otherwise look like a merely noisy render.
        u32 ConsecutiveRestarts = 0;
        // Live punctual lights at slots the shader's loop bound
        // (kGpuPathTracerMaxLights) cannot reach. They light the raster frame
        // and the CPU reference but not this tracer.
        u32 PunctualLightsBeyondShaderBound = 0;
        // The emissive table holds triangles but has no device address on this
        // backend, so next-event estimation sees no area lights.
        bool EmissiveTableUnaddressable = false;

        void Reset() noexcept
        {
            *this = GpuPathTracerStats{};
        }
    };

    // @brief Artist / diagnostic controls for the GPU reference path tracer.
    //
    // The defaults reproduce PathTracing::PathTracerSettings' defaults where
    // the two share a knob, so a scene traced by both tracers at the same
    // settings is the same integral: MaxBounces 8, Russian roulette from bounce
    // 4, NEE on, no radiance clamp, ray epsilon 1e-3, seed 0x9e3779b9.
    struct GpuPathTracerSettings
    {
        bool Enabled = false;

        // Samples added per pixel per frame. Progressive: the frame budget is
        // this times the path length, and the image converges over frames.
        u32 SamplesPerFrame = 1;
        // Stop adding samples once every pixel has this many. 0 = unbounded.
        // A fixed cap is what makes a run reproducible: N frames at 1 spp and
        // 1 frame at N spp accumulate the SAME sample indices.
        u32 MaxSamples = 0;

        // Number of SURFACE interactions along a path. 1 == direct lighting
        // only (no GI); 2 == one bounce of indirect, and so on. Clamped to
        // GpuPathTracer.glsl's OLO_PT_MAX_BOUNCES (32).
        u32 MaxBounces = 8;
        // Start Russian roulette after this many bounces. 0 disables RR, which
        // a furnace test wants — the whole point there is to not throw away
        // energy stochastically at low depth.
        u32 RussianRouletteStartBounce = 4;
        // Global sampler seed. Changing it changes the noise but not the
        // converged value.
        u32 Seed = 0x9e3779b9u;

        // Next-event estimation. Off makes the integrator pure BSDF sampling —
        // still unbiased, far noisier, and the cross-check that proves the NEE
        // + MIS machinery did not introduce a bias.
        bool EnableNextEventEstimation = true;
        // Firefly clamp on a single path's contribution, in radiance units.
        // <= 0 disables it. A clamp is a BIAS — it stays off for anything that
        // claims to be ground truth; it exists for eyeballing a noisy preview.
        f32 MaxRadianceClamp = 0.0f;
        // Ray offset along the geometric normal to avoid self-intersection.
        f32 RayEpsilon = 1e-3f;
        // Metres before a closest-hit ray is treated as escaped. The CPU
        // reference traces to infinity; a finite bound only matters for a
        // scene larger than it.
        f32 MaxRayDistance = 1.0e5f;

        // The environment collected on escape: a uniform radiance (the only
        // environment the CPU reference supports, and the furnace lever) plus
        // the frame's environment cube at LOD 0 scaled by the intensity. Cube
        // intensity 0 leaves the uniform term alone, which is what a
        // CPU-vs-GPU parity run wants.
        glm::vec3 UniformEnvironmentRadiance{ 0.0f };
        f32 EnvironmentCubeIntensity = 1.0f;

        // What lands in the colour the post chain consumes.
        GpuPathTracerDebugView DebugView = GpuPathTracerDebugView::Radiance;
        // Display scales for the two unbounded AOV views.
        f32 SampleCountDisplayScale = 1.0f / 256.0f;
        f32 VarianceDisplayScale = 100.0f;

        // Required, not optional: PostProcessSettings holds this struct by value
        // and defaults its OWN operator==, so without one here that operator is
        // implicitly DELETED and the editor's undo-tracking snapshot compare
        // stops compiling. The float members are compared bitwise for CHANGE
        // DETECTION only, never to ask whether two settings are physically
        // equivalent, which is the distinction CLAUDE.md's no-`==`-on-floats
        // rule is about.
        bool operator==(const GpuPathTracerSettings&) const = default;
    };

    // The compile-time limits the shader's loops are bounded by — the
    // OLO_PT_MAX_* macros in GpuPathTracer.glsl, pinned equal by
    // GpuPathTracerContractTest. The pass clamps the settings to the first two
    // before upload, and the light slot count to the third, counting what the
    // clamp cut off; a value past them would silently trace less than asked.
    inline constexpr u32 kGpuPathTracerMaxBounces = 32u;
    inline constexpr u32 kGpuPathTracerMaxSamplesPerFrame = 64u;
    inline constexpr u32 kGpuPathTracerMaxLights = 256u;

    // The bits of u_EmissiveTable.w — the OLO_PT_FLAG_* macros in the shader,
    // pinned by the same test. The pass sets them; the device parity test
    // sets them by hand to drive the shader without the pass.
    inline constexpr u32 kGpuPathTracerFlagNextEventEstimation = 1u;
    inline constexpr u32 kGpuPathTracerFlagEnvironmentCube = 2u;
    inline constexpr u32 kGpuPathTracerFlagHistoryValid = 4u;

    // The range every knob is held to, in ONE place: the scene loader, the
    // pass's upload, the MCP registry and the editor panel all take their
    // bounds from here, so a value cannot be legal on one path and clamped on
    // another.
    namespace GpuPathTracerLimits
    {
        // The count lives in the accumulation plane's alpha as an f32, exact
        // up to 2^24; a cap past that could never be reached exactly.
        inline constexpr u32 kMaxSamplesCap = 1u << 24u;
        inline constexpr f32 kMinRayEpsilon = 1.0e-6f;
        inline constexpr f32 kMaxRayEpsilon = 1.0f;
        inline constexpr f32 kMinRayDistance = 1.0f;
        inline constexpr f32 kMaxRayDistance = 1.0e7f;
        inline constexpr f32 kMaxRadianceClamp = 1.0e6f;
        inline constexpr f32 kMaxUniformEnvironmentRadiance = 1.0e4f;
        inline constexpr f32 kMaxEnvironmentCubeIntensity = 100.0f;
        inline constexpr f32 kMaxSampleCountDisplayScale = 1.0f;
        inline constexpr f32 kMaxVarianceDisplayScale = 1.0e6f;
    } // namespace GpuPathTracerLimits

    // The settings as the pass may use them: every integer inside the shader's
    // loop bounds, every float finite and inside its range, a non-finite float
    // replaced by the default rather than by a clamp of garbage. Applied on
    // scene load and again before every upload, because values also arrive
    // from a live edit, a script or an MCP write and the shader cannot be the
    // backstop.
    [[nodiscard]] inline GpuPathTracerSettings SanitizeGpuPathTracerSettings(const GpuPathTracerSettings& in) noexcept
    {
        using namespace GpuPathTracerLimits;
        const GpuPathTracerSettings defaults{};
        const auto finiteOr = [](f32 value, f32 fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        };
        const auto clampFinite = [&](f32 value, f32 fallback, f32 lo, f32 hi) noexcept
        {
            return std::clamp(finiteOr(value, fallback), lo, hi);
        };

        GpuPathTracerSettings s = in;
        s.SamplesPerFrame = std::clamp(s.SamplesPerFrame, 1u, kGpuPathTracerMaxSamplesPerFrame);
        s.MaxSamples = std::min(s.MaxSamples, kMaxSamplesCap);
        s.MaxBounces = std::clamp(s.MaxBounces, 1u, kGpuPathTracerMaxBounces);
        s.RussianRouletteStartBounce = std::min(s.RussianRouletteStartBounce, kGpuPathTracerMaxBounces);
        s.MaxRadianceClamp = clampFinite(s.MaxRadianceClamp, defaults.MaxRadianceClamp, 0.0f, kMaxRadianceClamp);
        s.RayEpsilon = clampFinite(s.RayEpsilon, defaults.RayEpsilon, kMinRayEpsilon, kMaxRayEpsilon);
        s.MaxRayDistance = clampFinite(s.MaxRayDistance, defaults.MaxRayDistance, kMinRayDistance, kMaxRayDistance);
        for (int i = 0; i < 3; ++i)
        {
            s.UniformEnvironmentRadiance[i] = clampFinite(s.UniformEnvironmentRadiance[i],
                                                          defaults.UniformEnvironmentRadiance[i], 0.0f,
                                                          kMaxUniformEnvironmentRadiance);
        }
        s.EnvironmentCubeIntensity = clampFinite(s.EnvironmentCubeIntensity, defaults.EnvironmentCubeIntensity, 0.0f,
                                                 kMaxEnvironmentCubeIntensity);
        if (std::to_underlying(s.DebugView) >= std::to_underlying(GpuPathTracerDebugView::Count))
            s.DebugView = GpuPathTracerDebugView::Radiance;
        s.SampleCountDisplayScale = clampFinite(s.SampleCountDisplayScale, defaults.SampleCountDisplayScale, 0.0f,
                                                kMaxSampleCountDisplayScale);
        s.VarianceDisplayScale =
            clampFinite(s.VarianceDisplayScale, defaults.VarianceDisplayScale, 0.0f, kMaxVarianceDisplayScale);
        return s;
    }

} // namespace OloEngine
