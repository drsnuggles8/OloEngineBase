#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphPlanExecutor.h"

#include "OloEngine/Debug/Profiler.h"

#include <chrono>

namespace OloEngine::RenderGraphPlanExecutor
{
    auto ExecutePlan(const ExecuteInput& input) -> std::vector<RenderGraph::ExecutionTiming>
    {
        OLO_PROFILE_FUNCTION();

        using SubmissionCommand = RenderGraph::SubmissionCommand;
        using ExecutionTiming = RenderGraph::ExecutionTiming;

        std::vector<ExecutionTiming> timings;
        timings.reserve(input.SubmissionPlan.size());

        // Each command kind maps to a distinct action; barrier placement and
        // async-compute batch boundaries are encoded in the plan so this
        // loop requires no topology lookups or per-frame map probes.
        for (const auto& cmd : input.SubmissionPlan)
        {
            switch (cmd.CommandKind)
            {
                case SubmissionCommand::Kind::BatchBegin:
                {
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
                case SubmissionCommand::Kind::MemoryBarrier:
                {
                    if (input.RuntimeBarrierExecutionEnabled)
                        input.Context.MemoryBarrier(cmd.Barriers);
                    break;
                }
                case SubmissionCommand::Kind::Pass:
                {
                    if (!input.IsPassReachable(cmd.NodeName))
                        break;

                    if (!cmd.NodePointer)
                        break;

                    input.Context.BeginPass(cmd.NodeName);
                    const auto executeStart = std::chrono::steady_clock::now();
                    cmd.NodePointer->Execute(input.Context);
                    const auto executeEnd = std::chrono::steady_clock::now();
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

        return timings;
    }
} // namespace OloEngine::RenderGraphPlanExecutor
