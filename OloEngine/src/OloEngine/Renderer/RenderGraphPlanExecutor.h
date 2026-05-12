#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::RenderGraphPlanExecutor
{
    // Backend executor extracted from the IR walk loop in
    // `RenderGraph::Execute()` as part of the Phase 7 split (2026-05-12).
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
    };

    // Runs the IR walk and returns per-pass CPU timings (one entry per
    // executed pass; passes that were skipped because they failed the
    // reachability predicate produce no entry).
    [[nodiscard]] auto ExecutePlan(const ExecuteInput& input) -> std::vector<RenderGraph::ExecutionTiming>;
} // namespace OloEngine::RenderGraphPlanExecutor
