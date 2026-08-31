#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphPlanExecutor.h"

#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHIGpuFence.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>
#include <vector>

namespace OloEngine::RenderGraphPlanExecutor
{
    auto ExecutePlan(const ExecuteInput& input) -> std::vector<RenderGraph::ExecutionTiming>
    {
        OLO_PROFILE_FUNCTION();

        using SubmissionCommand = RenderGraph::SubmissionCommand;
        using ExecutionTiming = RenderGraph::ExecutionTiming;

        std::vector<ExecutionTiming> timings;
        timings.reserve(input.SubmissionPlan.size());

        // One fence/value per immutable plan edge. The plan is backend-neutral;
        // a concrete GpuFence exists only while a backend can actually submit
        // the producer segment separately from the consumer segment. Keeping
        // the normal MemoryBarrier commands in the plan is intentional: a
        // timeline wait supplies submission ordering, not resource visibility
        // or image-layout transition work (ADR 0011 §6).
        struct RuntimeFence
        {
            Ref<RHI::GpuFence> Fence;
            u64 Value = 0;
        };
        bool splitSubmissionEnabled = input.SupportsFenceSubmission
                                          ? input.SupportsFenceSubmission()
                                          : input.Context.SupportsFenceSubmission();
        // Fence indices are dense plan-local IDs. A vector avoids hashing and
        // node allocation on every frame; it stays empty for the overwhelmingly
        // common no-split plan. Reachability is cached because each edge is
        // visited once at its signal and again at its wait.
        std::vector<RuntimeFence> runtimeFences;
        std::vector<i8> fenceReachability;
        bool hasPendingFenceSignals = false;
        bool hasPendingFenceWaits = false;
        const auto fenceEdgeIsReachable = [&input, &fenceReachability](const RenderGraph::FenceEdge& edge)
        {
            if (fenceReachability.size() <= edge.Index)
                fenceReachability.resize(static_cast<sizet>(edge.Index) + 1u, static_cast<i8>(-1));
            i8& cached = fenceReachability[edge.Index];
            if (cached < 0)
            {
                cached = static_cast<i8>(input.IsPassReachable(edge.ProducerPass) &&
                                         input.IsPassReachable(edge.ConsumerPass));
            }
            return cached != 0;
        };

        // Submit all commands accumulated up to the next consumer boundary.
        // Keeping a signal staged until that boundary lets one submission
        // cover several producer passes, while ensuring an async-batch debug
        // label never spans two Vulkan command buffers. Wait-only work normally
        // stays attached to a later producer segment; the final call opts in to
        // flushing it so the per-execution fence objects outlive their submit.
        const auto submitPendingFenceOps = [&](const bool includeWaitOnly = false)
        {
            if (!splitSubmissionEnabled || (!hasPendingFenceSignals && !(includeWaitOnly && hasPendingFenceWaits)))
                return;

            const bool submitted = input.SubmitFenceSegment
                                       ? input.SubmitFenceSegment()
                                       : input.Context.SubmitFenceSegment();
            if (!submitted)
            {
                OLO_CORE_WARN("RenderGraph: backend declined the split-barrier producer submit; "
                              "falling back to full barriers for this frame");
                splitSubmissionEnabled = false;
            }
            hasPendingFenceSignals = false;
            hasPendingFenceWaits = false;
        };

        // Each command kind maps to a distinct action; barrier placement and
        // async-compute batch boundaries are encoded in the plan so this
        // loop requires no topology lookups or per-frame map probes.
        for (const auto& cmd : input.SubmissionPlan)
        {
            switch (cmd.CommandKind)
            {
                case SubmissionCommand::Kind::BatchBegin:
                {
                    // A batch opens a backend debug scope. Flush before doing
                    // that so the scope begins and ends in one command buffer.
                    submitPendingFenceOps();
                    input.Context.BeginAsyncBatch(cmd.BatchIndex);
                    if (input.BatchEventHook)
                        input.BatchEventHook(cmd.BatchIndex, true);
                    break;
                }
                case SubmissionCommand::Kind::BatchEnd:
                {
                    input.Context.EndAsyncBatch(cmd.BatchIndex);
                    if (input.BatchEventHook)
                        input.BatchEventHook(cmd.BatchIndex, false);
                    break;
                }
                case SubmissionCommand::Kind::FenceWait:
                {
                    if (!splitSubmissionEnabled)
                        break;

                    // The immutable plan also contains branches culled for
                    // this execution. They own no producer or consumer work
                    // and therefore need no runtime semaphore operation.
                    if (std::ranges::none_of(cmd.FenceEdges, fenceEdgeIsReachable))
                        break;

                    // The preceding producer must be a distinct submission
                    // before its timeline value can be waited on. BatchEnd
                    // (when present) has already closed its debug scope.
                    submitPendingFenceOps();
                    if (!splitSubmissionEnabled)
                        break;

                    // Producer commands precede their consumers in the
                    // topological plan, so every edge should already have a
                    // staged value. If a backend declines a prior segment the
                    // whole fence path is disabled, leaving the normal barrier
                    // path as the conservative fallback rather than queuing an
                    // unsatisfiable wait.
                    bool canStage = true;
                    for (const auto& edge : cmd.FenceEdges)
                    {
                        if (!fenceEdgeIsReachable(edge))
                            continue;
                        if (edge.Index >= runtimeFences.size() || !runtimeFences[edge.Index].Fence ||
                            runtimeFences[edge.Index].Value == 0u)
                        {
                            OLO_CORE_WARN("RenderGraph: split-barrier wait for '{}' -> '{}' had no producer value; "
                                          "falling back to full barriers for this frame",
                                          edge.ProducerPass, edge.ConsumerPass);
                            canStage = false;
                            break;
                        }
                    }
                    if (!canStage)
                    {
                        splitSubmissionEnabled = false;
                        break;
                    }

                    bool stagedWait = false;
                    for (const auto& edge : cmd.FenceEdges)
                    {
                        if (!fenceEdgeIsReachable(edge))
                            continue;
                        auto& runtime = runtimeFences[edge.Index];
                        runtime.Fence->QueueWait(runtime.Value);
                        stagedWait = true;
                    }
                    hasPendingFenceWaits = hasPendingFenceWaits || stagedWait;
                    break;
                }
                case SubmissionCommand::Kind::FenceSignal:
                {
                    if (!splitSubmissionEnabled)
                        break;

                    // Do all construction before staging any signal: if a
                    // device cannot create one timeline semaphore, an earlier
                    // edge in this fan-out must not be left pending against a
                    // submission the executor then declines to make.
                    bool canStage = true;
                    bool stagedSignal = false;
                    for (const auto& edge : cmd.FenceEdges)
                    {
                        if (!fenceEdgeIsReachable(edge))
                            continue;

                        if (runtimeFences.size() <= edge.Index)
                            runtimeFences.resize(static_cast<sizet>(edge.Index) + 1u);
                        auto& runtime = runtimeFences[edge.Index];
                        if (!runtime.Fence)
                            runtime.Fence = input.CreateGpuFence ? input.CreateGpuFence() : RHI::GpuFence::Create();
                        if (!runtime.Fence)
                        {
                            OLO_CORE_WARN("RenderGraph: split-barrier signal for '{}' -> '{}' could not create a GPU fence; "
                                          "falling back to full barriers for this frame",
                                          edge.ProducerPass, edge.ConsumerPass);
                            canStage = false;
                            break;
                        }
                    }
                    if (!canStage)
                    {
                        splitSubmissionEnabled = false;
                        break;
                    }

                    for (const auto& edge : cmd.FenceEdges)
                    {
                        if (!fenceEdgeIsReachable(edge))
                            continue;

                        auto& runtime = runtimeFences[edge.Index];
                        const u64 value = runtime.Fence->NextValue();
                        if (value == std::numeric_limits<u64>::max())
                        {
                            OLO_CORE_ERROR("RenderGraph: split-barrier fence for '{}' -> '{}' exhausted its timeline; "
                                           "falling back to full barriers for this frame",
                                           edge.ProducerPass, edge.ConsumerPass);
                            splitSubmissionEnabled = false;
                            break;
                        }
                        runtime.Value = value;
                        runtime.Fence->QueueSignal(value);
                        stagedSignal = true;
                    }
                    hasPendingFenceSignals = hasPendingFenceSignals || (splitSubmissionEnabled && stagedSignal);
                    break;
                }
                case SubmissionCommand::Kind::MemoryBarrier:
                {
                    if (input.RuntimeBarrierExecutionEnabled)
                    {
                        std::vector<RHI::Barrier> resolved;
                        if (input.GraphForBarrierResolution && !cmd.Transitions.empty())
                            resolved = input.GraphForBarrierResolution->ResolveTransitionsToBarriers(cmd.Transitions);
                        input.Context.IssueBarrierBatch(cmd.Barriers, resolved);
                    }
                    break;
                }
                case SubmissionCommand::Kind::Pass:
                {
                    if (!input.IsPassReachable(cmd.NodeName))
                        break;

                    if (!cmd.NodePointer)
                        break;

                    input.Context.BeginPass(cmd.NodeName);
                    // Always-on per-pass GPU timestamps (GL_TIMESTAMP pairs, so
                    // they coexist with the capture path's per-draw
                    // GL_TIME_ELAPSED scopes inside the pass). Resolved a few
                    // frames later by GPUPassTimerPool::BeginFrame; surfaced via
                    // the olo_perf_pass_timings MCP tool.
                    auto& gpuTimers = GPUPassTimerPool::GetInstance();
                    gpuTimers.BeginPass(cmd.NodeName);
                    // Name the pass in the backend's command stream. This is
                    // what makes a GPU-side diagnostic say WHICH pass: a
                    // RenderDoc/Nsight region, and — on Vulkan — the
                    // command-buffer label region the validation layer reports
                    // alongside an error. Issue #800 spent two phases
                    // narrowing a per-resize layout error by inference
                    // precisely because no such label existed. The backends
                    // no-op when the capability is absent.
                    // Scoped, not a bare push/pop pair: an Execute that
                    // throws past an unbalanced push leaves the label region
                    // open, and every later error is then attributed to this
                    // pass — the exact failure this label exists to prevent.
                    struct DebugGroupScope
                    {
                        explicit DebugGroupScope(std::string_view name)
                        {
                            RenderCommand::PushDebugGroup(0u, name);
                        }
                        ~DebugGroupScope()
                        {
                            RenderCommand::PopDebugGroup();
                        }
                        DebugGroupScope(const DebugGroupScope&) = delete;
                        DebugGroupScope& operator=(const DebugGroupScope&) = delete;
                        DebugGroupScope(DebugGroupScope&&) = delete;
                        DebugGroupScope& operator=(DebugGroupScope&&) = delete;
                    };
                    std::chrono::steady_clock::time_point executeStart{};
                    std::chrono::steady_clock::time_point executeEnd{};
                    {
                        const DebugGroupScope debugGroup{ cmd.NodeName };
                        executeStart = std::chrono::steady_clock::now();
                        cmd.NodePointer->Execute(input.Context);
                        executeEnd = std::chrono::steady_clock::now();
                    }
                    gpuTimers.EndPass();
                    input.Context.EndPass();

                    const auto elapsedMs = std::chrono::duration<f64, std::milli>(executeEnd - executeStart).count();
                    timings.push_back(ExecutionTiming{
                        .NodeName = cmd.NodeName,
                        .CpuMs = elapsedMs,
                    });

                    // Debug post-pass hook — fires after EndPass() but before
                    // the next pass begins. Lets debug tooling snapshot
                    // intermediate resource state (see RenderGraphFrameCapture).
                    if (input.PostPassHook && input.GraphForPostPassHook)
                        input.PostPassHook(cmd.NodeName, *input.GraphForPostPassHook);
                    break;
                }
            }
        }

        // A graph can end on either side of an edge. There is no following
        // boundary to force the last signal or wait out, and the runtime fence
        // objects below are intentionally scoped to this execution. Flush both
        // kinds here before those objects are released; later frame work resumes
        // in the backend's continuation command buffer.
        submitPendingFenceOps(/*includeWaitOnly=*/true);

        return timings;
    }
} // namespace OloEngine::RenderGraphPlanExecutor
