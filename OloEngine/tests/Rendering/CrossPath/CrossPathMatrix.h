#pragma once

// =============================================================================
// CrossPathMatrix.h — the contract of the cross-path lighting matrix
// (issue #1347, acceptance criteria 3 and 5).
// =============================================================================
//
// One parameterised fixture renders the same scenes through every declared
// {backend x rendering path x sampling/upscale} arm and compares SEPARATED
// lighting terms across arms and against analytic expectations. A new
// feature adds a scene row; it does not hand-check six paths.
//
// ARMS come from the support registry, never from a hand-written list:
// `EnumerateArms()` walks RendererSupport::PathCoverageRows (the executable
// support matrix of #1334 / #1438). A row the registry declares Unsupported
// is still an arm — it SKIPS, with the registry's reason, so the declared
// matrix and the tested matrix cannot drift apart silently.
//
// An arm that cannot run in this process skips LOUDLY with the reason. Every
// skip reason is a fact about the world:
//   * Unsupported in the registry -> the registry's Reason.
//   * Vulkan -> "scene-level Vulkan is unreachable in-process": one process
//     holds one backend, and Scene::OnUpdateRuntime needs the GL Renderer3D
//     statics a Vulkan swap tears down (testing-architecture.md §9). Those
//     cells are run live against the editor by
//     scripts/cross-path-matrix-live.py, which renders the same rows.
//   * no GL 4.6 context (CI software driver) -> the fixture's GPU gate.
//
// SEPARATED TERMS. The lighting-signal AOVs of #1336 are an ownership table,
// not buffers, and the per-term debug views exist on Deferred only. So a row
// separates a term the way a physicist would: it renders the scene twice,
// with that one term's SOURCE off and on, and differences the linear HDR
// SceneColor. With every other term held fixed, ON - OFF is exactly that
// term's contribution, on every path, with no path-specific instrumentation
// to diverge. A row's scene is designed so the difference contains one term
// (a black dielectric has no diffuse; an unlit black surface has no direct
// term; an emitter-free scene has no emission).
//
// OWNERSHIP. A probe may name the estimator that must own its term (SSGI,
// ReSTIR DI/GI/PT, SSR). The arm consults ResolveLightingSignalOwnership —
// the same function the passes are configured from — and skips with the
// contract's answer when that path does not host the estimator, instead of
// hard-coding "Deferred only" a second time.
//
// Written for GL arms in-process; the row/probe declarations carry no GL.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/LightingSignalContract.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Support/RendererSupport.h"
#include "OloEngine/Renderer/Support/RendererSupportRows.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class Scene;
}

namespace OloEngine::Tests::CrossPath
{
    // ---- arms ---------------------------------------------------------------

    struct Arm
    {
        std::string_view Id; // the support-matrix row id, e.g. "gl-forward-native"
        RendererSupport::Request Request;
        RendererSupport::Capabilities Device;
        RendererSupport::Decision Declared; // Evaluate(Request, Device) — what the registry says
    };

    [[nodiscard]] inline std::vector<Arm> EnumerateArms()
    {
        std::vector<Arm> arms;
        for (const RendererSupport::CoverageRow& row : RendererSupport::PathCoverageRows)
            arms.push_back({ row.Id, row.Requested, row.Device, RendererSupport::Evaluate(row.Requested, row.Device) });
        return arms;
    }

    // The arm every other arm is compared against: GL Forward at native
    // resolution, the simplest path (the light loop, no G-Buffer, no tiles).
    inline constexpr std::string_view kReferenceArmId = "gl-forward-native";

    [[nodiscard]] inline std::string_view ToString(RenderingPath path)
    {
        switch (path)
        {
            case RenderingPath::Forward:
                return "Forward";
            case RenderingPath::ForwardPlus:
                return "ForwardPlus";
            case RenderingPath::Deferred:
                return "Deferred";
        }
        return "Unknown";
    }

    // The part of the evidence filename that names the arm:
    // GL_Forward, GL_Deferred_MSAA4, GL_Deferred_Spatial_MSAA4, ...
    [[nodiscard]] inline std::string CellName(const Arm& arm)
    {
        std::string name = arm.Request.Api == RendererSupport::Backend::Vulkan ? "Vulkan_" : "GL_";
        name += ToString(arm.Request.Path);
        if (arm.Request.Upscale == RendererSupport::Reconstruction::Spatial)
            name += "_Spatial";
        else if (arm.Request.Upscale == RendererSupport::Reconstruction::Temporal)
            name += "_Temporal";
        if (arm.Request.Samples > 1u)
            name += "_MSAA" + std::to_string(arm.Request.Samples);
        if (arm.Request.LightingTechnique != RendererSupport::Technique::Raster)
            name += "_RayQuery";
        return name;
    }

    struct ArmPlan
    {
        bool Runs = false;
        std::string SkipReason; // set when !Runs; a fact, printed by the skip
    };

    [[nodiscard]] inline ArmPlan PlanArm(const Arm& arm)
    {
        if (arm.Declared.Status == RendererSupport::Outcome::Unsupported)
            return { false, "the support registry declares " + std::string(arm.Id) + " Unsupported (" +
                                std::string(RendererSupport::ToString(arm.Declared.Why)) +
                                ") — the arm exists so the declared and tested matrices cannot drift" };
        if (arm.Request.Api == RendererSupport::Backend::Vulkan)
            return { false, std::string(arm.Id) +
                                ": scene-level Vulkan is unreachable in-process — one process holds one backend and "
                                "Scene::OnUpdateRuntime needs the GL Renderer3D statics a Vulkan swap tears down "
                                "(docs/agent-rules/testing-architecture.md §9). Live cell: "
                                "scripts/cross-path-matrix-live.py renders the same rows in the editor with --rhi vulkan" };
        if (arm.Request.LightingTechnique != RendererSupport::Technique::Raster)
            return { false, std::string(arm.Id) + ": a ray-query arm needs a hardware ray-tracing Vulkan device" };
        return { true, {} };
    }

    // ---- term probes and scene rows ---------------------------------------

    // Linear HDR radiance the analytic model predicts at one world-space point
    // seen from `eye` (per pixel, so a perspective camera's varying view
    // direction is part of the prediction). nullopt = no prediction here.
    using AnalyticFn = std::function<std::optional<glm::dvec3>(const glm::dvec3& worldPoint, const glm::dvec3& eye)>;

    // A tolerance with the reason it has that value. Held per region mean:
    // |a - b| <= Relative * |b| + Absolute, per channel.
    struct Tolerance
    {
        f64 Relative = 0.0;
        f64 Absolute = 0.0;
        std::string_view Reason;
    };

    // A rectangle on the y = 0 ground plane (x, z extents), measured as a
    // region of the frame: the pixels whose ground point falls inside it.
    struct GroundRegion
    {
        std::string_view Name;
        glm::dvec2 Min; // (x, z)
        glm::dvec2 Max;
        AnalyticFn Analytic; // may be empty: cross-arm comparison only
    };

    // One separated term: `SetSource(scene, on)` switches the term's source,
    // and ON - OFF is that term alone.
    struct TermProbe
    {
        std::string_view Name;
        LightingTerm Term = LightingTerm::DirectDiffuse;
        std::function<void(Scene&, bool on)> SetSource;
        std::vector<GroundRegion> Regions;
        Tolerance Analytic; // against the region's AnalyticFn
        Tolerance CrossArm; // against the reference arm
        // Replaces CrossArm on a temporally reconstructed arm, when the term is
        // one the reconstruction itself changes (a sub-pixel HDR peak), with
        // its own reason. Empty: CrossArm holds there too.
        std::optional<Tolerance> TemporalCrossArm;
        // A region's ON - OFF mean must exceed this (per its brightest channel)
        // on the reference arm — so two arms cannot agree by both drawing nothing.
        f64 MinimumSignal = 1.0e-3;
    };

    // An estimator switched on and off while a term is held: the term's
    // separated value must not move (the estimator does not own it), or must
    // move (the positive control that the estimator ran on this arm).
    struct EstimatorToggle
    {
        std::string_view Name; // "GTAO", "SSGI", ...
        std::function<void(bool on)> Set;
        LightingEstimator Estimator = LightingEstimator::None; // None: not an ownership estimator (AO)
        // The contract configuration that makes Estimator live, used to ask
        // ResolveLightingSignalOwnership whether this arm hosts it.
        std::function<void(LightingFrameConfiguration&)> Engage;
        // The olo_postprocess_settings_set field that switches it in a live
        // editor ("SSGIEnabled", "ReSTIRDIEnabled"); empty when not needed.
        std::string_view McpField;
    };

    struct InvarianceProbe
    {
        std::string_view TermProbeName; // which TermProbe of the row to hold
        EstimatorToggle Toggle;
        bool ExpectChange = false; // false: invariance; true: positive control
        Tolerance Allowed;         // invariance bound, or minimum change as Absolute
    };

    struct SceneRow
    {
        std::string_view Name;
        std::string_view Motivation; // the issue it came from
        std::function<void(Scene&)> Build;
        std::vector<TermProbe> Probes;
        std::vector<InvarianceProbe> Invariances;
        // Row-wide requirement: the estimator every probe needs (None: any path).
        EstimatorToggle Requires{};
    };

    // Does this arm's path host `toggle`'s estimator for `term`? Answered by the
    // lighting-signal contract, not re-derived here.
    [[nodiscard]] inline std::optional<std::string> OwnershipSkip(const Arm& arm, const EstimatorToggle& toggle,
                                                                  LightingTerm term)
    {
        if (toggle.Estimator == LightingEstimator::None || !toggle.Engage)
            return std::nullopt;
        LightingFrameConfiguration frame;
        frame.Path = arm.Request.Path;
        toggle.Engage(frame);
        const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);
        if (HasEstimator(ownership.Of(term).Owners, toggle.Estimator))
            return std::nullopt;
        return "the lighting-signal contract gives " + std::string(ToString(term)) + " no " + std::string(toggle.Name) +
               " owner on " + std::string(ToString(arm.Request.Path)) +
               " (ResolveLightingSignalOwnership) — this path does not host the estimator";
    }

    // Estimators that need a hardware ray-tracing device (Vulkan) — every GL
    // arm skips them with that reason.
    [[nodiscard]] inline bool NeedsRayQueries(LightingEstimator e)
    {
        return e == LightingEstimator::ReSTIRDI || e == LightingEstimator::ReSTIRGI ||
               e == LightingEstimator::ReSTIRPT || e == LightingEstimator::RayTracedReflection;
    }
} // namespace OloEngine::Tests::CrossPath
