#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIGpuFence.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace OloEngine
{
    class RendererAPI;
}

namespace OloEngine::RenderGraphPlanExecutor
{
    // Backend executor extracted from the IR walk loop in
    // `RenderGraph::Execute()` as part of the module split (2026-05-12).
    // Now that the submission plan is its own module
    // (`RenderGraphSubmissionPlan`), the executor is a thin loop over the
    // precomputed plan that dispatches each command to the abstract
    // `RGCommandContext`. No backend-specific code lives here — the OpenGL
    // bindings stay one level deeper, inside the `RGCommandContext` /
    // `OpenGLRendererAPI` chain.

    struct ExecuteInput
    {
        std::span<const RenderGraph::SubmissionCommand> SubmissionPlan;
        RGCommandContext& Context;
        bool RuntimeBarrierExecutionEnabled = true;
        // Predicate: returns true when the named pass survived culling.
        std::function<bool(const std::string&)> IsPassReachable;
        // Optional batch-event hook (fires on each `BatchBegin` / `BatchEnd`).
        RenderGraph::BatchEventCallback BatchEventHook;
        // Optional post-pass hook (fires after every `EndPass`, before the
        // next command). The graph reference is forwarded to the hook for
        // debug-tooling use (e.g. `RenderGraphFrameCapture`).
        RenderGraph::PostPassHook PostPassHook;
        RenderGraph* GraphForPostPassHook = nullptr;

        // ADR 0011 §1.5: when set, each MemoryBarrier command's
        // name-keyed transitions are resolved to handle-keyed RHI::Barriers
        // at execute time (transient physicals change per frame, so this
        // cannot be baked into the plan) and passed to the context alongside
        // the GL flags. When null — headless plan-shape tests — the barrier
        // batch carries flags only, which is the complete GL behaviour.
        RenderGraph* GraphForBarrierResolution = nullptr;

        // Optional owner-specific submission adapter. The live renderer uses
        // RGCommandContext; a headless/offscreen owner can supply the same two
        // operations for its own command-buffer chain without a platform
        // downcast in this backend-neutral executor.
        std::function<bool()> SupportsFenceSubmission;
        std::function<bool()> SubmitFenceSegment;
        // Test/alternate-owner seam for the otherwise backend-selected fence
        // factory. Production leaves this empty and uses GpuFence::Create().
        std::function<Ref<RHI::GpuFence>()> CreateGpuFence;
        // Alternate/offscreen owner of the recording command stream. The live
        // renderer uses the facade when this is null, as for fence submission.
        RendererAPI* RecordingAPI = nullptr;
    };

    // Runs the IR walk and returns per-pass CPU timings (one entry per
    // executed pass; passes that were skipped because they failed the
    // reachability predicate produce no entry).
    [[nodiscard]] auto ExecutePlan(const ExecuteInput& input) -> std::vector<RenderGraph::ExecutionTiming>;
} // namespace OloEngine::RenderGraphPlanExecutor
