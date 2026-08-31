#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphSubmissionPlan.h"

#include "OloEngine/Debug/Profiler.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace OloEngine::RenderGraphSubmissionPlan
{
    namespace
    {
        [[nodiscard]] auto MapWorkTypeToLane(RenderGraphPassWorkType workType) -> RenderGraph::QueueLane
        {
            switch (workType)
            {
                case RenderGraphPassWorkType::Compute:
                    return RenderGraph::QueueLane::Compute;
                case RenderGraphPassWorkType::Copy:
                    return RenderGraph::QueueLane::Copy;
                case RenderGraphPassWorkType::Graphics:
                default:
                    return RenderGraph::QueueLane::Graphics;
            }
        }
    } // namespace

    auto ComputeBatches(const BatchesInput& input) -> std::vector<RenderGraph::AsyncComputeBatch>
    {
        OLO_PROFILE_FUNCTION();

        using AsyncComputeBatch = RenderGraph::AsyncComputeBatch;
        using BatchResourceDependency = RenderGraph::BatchResourceDependency;

        std::vector<AsyncComputeBatch> batches;

        // Step 1: group consecutive AsyncComputeCandidate passes.
        AsyncComputeBatch current;
        for (const auto& passName : input.ExecutionOrder)
        {
            const bool isCandidate = input.IsGraphEntryAsyncComputeCandidate(passName);
            if (isCandidate)
            {
                current.ComputeNodes.push_back(passName);
            }
            else
            {
                if (!current.ComputeNodes.empty())
                {
                    batches.push_back(std::move(current));
                    current = {};
                }
            }
        }
        if (!current.ComputeNodes.empty())
            batches.push_back(std::move(current));

        if (batches.empty())
            return batches;

        // Step 2: build successor map (A → passes that depend on A).
        std::unordered_set<std::string_view> passSet;
        passSet.reserve(input.ExecutionOrder.size());
        for (const auto& name : input.ExecutionOrder)
            passSet.insert(name);

        std::unordered_map<std::string, std::vector<std::string>> successors;
        successors.reserve(input.ExecutionOrder.size());
        for (const auto& name : input.ExecutionOrder)
        {
            const auto depIt = input.Dependencies.find(name);
            if (depIt == input.Dependencies.end())
                continue;
            for (const auto& dep : depIt->second)
            {
                if (passSet.contains(dep))
                    successors[dep].push_back(name);
            }
        }

        // Step 3: fill WaitNodes / SignalNodes for each batch.
        for (auto& batch : batches)
        {
            std::unordered_set<std::string> batchSet(batch.ComputeNodes.begin(),
                                                     batch.ComputeNodes.end());
            std::unordered_set<std::string> waitSet;
            std::unordered_set<std::string> signalSet;

            for (const auto& computePass : batch.ComputeNodes)
            {
                // WaitNodes: direct predecessors not in this batch.
                if (const auto depIt = input.Dependencies.find(computePass);
                    depIt != input.Dependencies.end())
                {
                    for (const auto& dep : depIt->second)
                    {
                        if (!batchSet.contains(dep) && passSet.contains(dep))
                            waitSet.insert(dep);
                    }
                }

                // SignalNodes: direct successors not in this batch.
                if (const auto sucIt = successors.find(computePass); sucIt != successors.end())
                {
                    for (const auto& succ : sucIt->second)
                    {
                        if (!batchSet.contains(succ))
                            signalSet.insert(succ);
                    }
                }
            }

            batch.WaitNodes = std::vector<std::string>(waitSet.begin(), waitSet.end());
            batch.SignalNodes = std::vector<std::string>(signalSet.begin(), signalSet.end());
        }

        // Step 4: fill InputResources / OutputResources for each batch.
        std::unordered_map<std::string, sizet> passOrderIndex;
        passOrderIndex.reserve(input.ExecutionOrder.size());
        for (sizet i = 0; i < input.ExecutionOrder.size(); ++i)
            passOrderIndex[input.ExecutionOrder[i]] = i;

        for (auto& batch : batches)
        {
            const std::unordered_set<std::string> batchSet(batch.ComputeNodes.begin(),
                                                           batch.ComputeNodes.end());

            sizet batchStart = input.ExecutionOrder.size();
            sizet batchEnd = 0;
            for (const auto& cp : batch.ComputeNodes)
            {
                if (const auto idxIt = passOrderIndex.find(cp); idxIt != passOrderIndex.end())
                {
                    batchStart = std::min(batchStart, idxIt->second);
                    batchEnd = std::max(batchEnd, idxIt->second);
                }
            }
            if (batchStart > batchEnd)
                continue;

            // Collect all resources read / written by batch passes.
            std::unordered_set<std::string> batchReadResources;
            std::unordered_set<std::string> batchWrittenResources;
            for (const auto& cp : batch.ComputeNodes)
            {
                if (const auto accessIt = input.PassAccessDeclarations.find(cp);
                    accessIt != input.PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (acc.IsWrite)
                            batchWrittenResources.insert(acc.ResourceName);
                        else
                            batchReadResources.insert(acc.ResourceName);
                    }
                }
            }

            // InputResources: scan passes before batchStart for the last
            // external writer of each batch-read resource.
            std::unordered_map<std::string, std::string> inputByResource;
            for (sizet i = 0; i < batchStart; ++i)
            {
                const auto& passName = input.ExecutionOrder[i];
                if (batchSet.contains(passName))
                    continue;
                if (const auto accessIt = input.PassAccessDeclarations.find(passName);
                    accessIt != input.PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (acc.IsWrite && batchReadResources.contains(acc.ResourceName))
                            inputByResource[acc.ResourceName] = passName; // last writer wins
                    }
                }
            }

            // OutputResources: scan passes after batchEnd for the first
            // external reader of each batch-written resource.
            std::unordered_map<std::string, std::string> outputByResource;
            for (sizet i = batchEnd + 1; i < input.ExecutionOrder.size(); ++i)
            {
                const auto& passName = input.ExecutionOrder[i];
                if (batchSet.contains(passName))
                    continue;
                if (const auto accessIt = input.PassAccessDeclarations.find(passName);
                    accessIt != input.PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (!acc.IsWrite && batchWrittenResources.contains(acc.ResourceName) &&
                            !outputByResource.contains(acc.ResourceName))
                        {
                            outputByResource[acc.ResourceName] = passName; // first reader wins
                        }
                    }
                }
            }

            batch.InputResources.reserve(inputByResource.size());
            for (const auto& [res, externalNode] : inputByResource)
                batch.InputResources.push_back({ res, externalNode });
            std::ranges::sort(batch.InputResources,
                              [](const BatchResourceDependency& a, const BatchResourceDependency& b)
                              { return a.ResourceName < b.ResourceName; });

            batch.OutputResources.reserve(outputByResource.size());
            for (const auto& [res, externalNode] : outputByResource)
                batch.OutputResources.push_back({ res, externalNode });
            std::ranges::sort(batch.OutputResources,
                              [](const BatchResourceDependency& a, const BatchResourceDependency& b)
                              { return a.ResourceName < b.ResourceName; });
        }

        return batches;
    }

    auto BuildPlan(const PlanInput& input) -> std::vector<RenderGraph::SubmissionCommand>
    {
        OLO_PROFILE_FUNCTION();

        using SubmissionCommand = RenderGraph::SubmissionCommand;
        using AsyncComputeBatch = RenderGraph::AsyncComputeBatch;

        std::vector<SubmissionCommand> plan;
        plan.reserve(input.ExecutionOrder.size() * 2); // rough upper bound

        // Build a set of passes that are members of some async batch so we
        // can quickly look up which batch (if any) a pass belongs to.
        std::unordered_map<u32, const AsyncComputeBatch*> batchByIndex;
        batchByIndex.reserve(input.Batches.size());
        for (u32 batchIdx = 0; batchIdx < static_cast<u32>(input.Batches.size()); ++batchIdx)
            batchByIndex.emplace(batchIdx, &input.Batches[batchIdx]);

        std::unordered_map<std::string, u32> passToBatch;
        for (u32 batchIdx = 0; batchIdx < static_cast<u32>(input.Batches.size()); ++batchIdx)
        {
            for (const auto& passName : input.Batches[batchIdx].ComputeNodes)
                passToBatch.emplace(passName, batchIdx);
        }

        // Map: passName → barrier flags from the compiled barrier plan.
        // Barriers are keyed on the pass AFTER which they should fire — i.e.
        // the consumer pass that triggered them. Insert before that pass.
        std::unordered_map<std::string, MemoryBarrierFlags> barrierForPass;
        for (const auto& planned : input.PlannedBarriers)
        {
            auto& flags = barrierForPass[planned.BeforePass];
            flags = flags | planned.Flags;
        }

        // Map: passName → deduplicated transition records (the
        // explicit-barrier currency). The planner emits one barrier per prior
        // writer, but for layout purposes only the LAST writer's state
        // matters — earlier writers are ordered transitively through the WAW
        // barriers between the writers themselves — so per-writer duplicates
        // collapse on (resource, range, from, to). Known caveat, recorded in
        // ADR 0011's barrier amendments: two prior writers touching DISJOINT
        // subresources of one resource collapse to the last writer's stage
        // and access masks; sync validation is the instrument that would
        // surface a real graph relying on that (a hardening item).
        std::unordered_map<std::string, std::vector<RenderGraph::ResourceTransition>> transitionsForPass;
        for (const auto& transition : input.Transitions)
        {
            auto& list = transitionsForPass[transition.ConsumerPass];
            const bool duplicate = std::ranges::any_of(
                list,
                [&transition](const RenderGraph::ResourceTransition& existing)
                {
                    return existing.ResourceName == transition.ResourceName &&
                           existing.Range == transition.Range &&
                           existing.FromAccess == transition.FromAccess &&
                           existing.ToAccess == transition.ToAccess;
                });
            if (!duplicate)
                list.push_back(transition);
        }

        // Build the concrete signal/wait edge set from the graph's complete
        // dependency map, not from the async-batch heuristic or only from
        // resource transitions. Ordering-only AddExecutionDependency edges are
        // real dependencies too. Resource transitions annotate an edge with
        // its resources; the normal MemoryBarrier remains in the plan because
        // timeline submission ordering supplements visibility/layout work, it
        // does not replace it (ADR 0011 §6).
        std::vector<RenderGraph::FenceEdge> fenceEdges;
        if (input.EnableSplitBarriers)
        {
            const auto passIsScheduled = [&input](const std::string& passName)
            {
                return std::ranges::contains(input.ExecutionOrder, passName);
            };
            const auto findOrAddEdge = [&fenceEdges, &input](const std::string& producerPass,
                                                             const std::string& consumerPass)
                -> RenderGraph::FenceEdge*
            {
                const auto producerLane = MapWorkTypeToLane(input.GetPassWorkType(producerPass));
                const auto consumerLane = MapWorkTypeToLane(input.GetPassWorkType(consumerPass));
                if (producerLane == consumerLane)
                    return nullptr;

                auto existing = std::ranges::find_if(
                    fenceEdges,
                    [&producerPass, &consumerPass](const RenderGraph::FenceEdge& edge)
                    {
                        return edge.ProducerPass == producerPass && edge.ConsumerPass == consumerPass;
                    });
                if (existing == fenceEdges.end())
                {
                    existing = fenceEdges.emplace(
                        fenceEdges.end(),
                        RenderGraph::FenceEdge{
                            .ProducerPass = producerPass,
                            .ConsumerPass = consumerPass,
                            .ProducerLane = producerLane,
                            .ConsumerLane = consumerLane,
                            .Resources = {},
                        });
                }
                return &*existing;
            };

            for (const auto& [consumerPass, producerPasses] : input.Dependencies)
            {
                if (!passIsScheduled(consumerPass))
                    continue;
                for (const auto& producerPass : producerPasses)
                {
                    if (passIsScheduled(producerPass))
                        findOrAddEdge(producerPass, consumerPass);
                }
            }

            for (const auto& transition : input.Transitions)
            {
                if (!transition.IsCrossLane || transition.ProducerPass.empty() ||
                    transition.ProducerPass == "external" || transition.ConsumerPass.empty() ||
                    !passIsScheduled(transition.ProducerPass) || !passIsScheduled(transition.ConsumerPass))
                {
                    continue;
                }

                if (auto* edge = findOrAddEdge(transition.ProducerPass, transition.ConsumerPass);
                    edge != nullptr && !std::ranges::contains(edge->Resources, transition.ResourceName))
                {
                    edge->Resources.push_back(transition.ResourceName);
                }
            }

            // The planner's vector is normally insertion-order stable; make
            // stability a contract nonetheless because fence values appear in
            // GPU captures and a nondeterministic diagnostic is no diagnostic.
            std::ranges::sort(
                fenceEdges,
                [](const RenderGraph::FenceEdge& a, const RenderGraph::FenceEdge& b)
                {
                    return std::tie(a.ProducerPass, a.ConsumerPass, a.ProducerLane, a.ConsumerLane) <
                           std::tie(b.ProducerPass, b.ConsumerPass, b.ProducerLane, b.ConsumerLane);
                });
            for (u32 index = 0; index < static_cast<u32>(fenceEdges.size()); ++index)
            {
                auto& edge = fenceEdges[index];
                edge.Index = index;
                std::ranges::sort(edge.Resources);
            }
        }

        std::unordered_map<std::string, std::vector<const RenderGraph::FenceEdge*>> waitsByPass;
        std::unordered_map<std::string, std::vector<const RenderGraph::FenceEdge*>> signalsByPass;
        for (const auto& edge : fenceEdges)
        {
            waitsByPass[edge.ConsumerPass].push_back(&edge);
            signalsByPass[edge.ProducerPass].push_back(&edge);
        }

        // Walk the execution order and emit commands.
        u32 currentBatch = std::numeric_limits<u32>::max();

        for (const auto& passName : input.ExecutionOrder)
        {
            const auto batchIt = passToBatch.find(passName);
            const bool inBatch = (batchIt != passToBatch.end());
            // Batch-boundary open.
            if (const u32 batchIdx = inBatch ? batchIt->second : std::numeric_limits<u32>::max(); inBatch && batchIdx != currentBatch)
            {
                // Close the previous batch (if any) before opening a new one.
                if (currentBatch != std::numeric_limits<u32>::max())
                {
                    SubmissionCommand end;
                    end.CommandKind = SubmissionCommand::Kind::BatchEnd;
                    end.BatchIndex = currentBatch;
                    end.Lane = RenderGraph::QueueLane::Compute;
                    plan.push_back(std::move(end));
                }

                SubmissionCommand begin;
                begin.CommandKind = SubmissionCommand::Kind::BatchBegin;
                begin.BatchIndex = batchIdx;
                begin.Lane = RenderGraph::QueueLane::Compute;
                if (const auto batchInfoIt = batchByIndex.find(batchIdx); batchInfoIt != batchByIndex.end())
                {
                    begin.WaitNodes = batchInfoIt->second->WaitNodes;
                    begin.InputResources = batchInfoIt->second->InputResources;
                }
                plan.push_back(std::move(begin));
                currentBatch = batchIdx;
            }

            // Batch-boundary close (returning to graphics after a batch).
            if (!inBatch && currentBatch != std::numeric_limits<u32>::max())
            {
                SubmissionCommand end;
                end.CommandKind = SubmissionCommand::Kind::BatchEnd;
                end.BatchIndex = currentBatch;
                end.Lane = RenderGraph::QueueLane::Compute;
                if (const auto batchInfoIt = batchByIndex.find(currentBatch); batchInfoIt != batchByIndex.end())
                {
                    end.SignalNodes = batchInfoIt->second->SignalNodes;
                    end.OutputResources = batchInfoIt->second->OutputResources;
                }
                plan.push_back(std::move(end));
                currentBatch = std::numeric_limits<u32>::max();
            }

            auto passWorkType = input.GetPassWorkType(passName);
            auto* nodePtr = input.ResolveNodePointer(passName);
            const auto passLane = MapWorkTypeToLane(passWorkType);

            // Queue waits attach to the consumer submission BEFORE its normal
            // barrier and pass body. The barrier still carries the resource
            // visibility/layout transition; the wait only orders distinct
            // submissions/queues.
            if (const auto waitIt = waitsByPass.find(passName); waitIt != waitsByPass.end())
            {
                SubmissionCommand wait;
                wait.CommandKind = SubmissionCommand::Kind::FenceWait;
                wait.Lane = passLane;
                wait.FenceEdges.reserve(waitIt->second.size());
                for (const auto* edge : waitIt->second)
                    wait.FenceEdges.push_back(*edge);
                plan.push_back(std::move(wait));
            }

            // Memory barrier before this pass (if any).
            if (const auto barIt = barrierForPass.find(passName); barIt != barrierForPass.end())
            {
                SubmissionCommand barrier;
                barrier.CommandKind = SubmissionCommand::Kind::MemoryBarrier;
                barrier.Barriers = barIt->second;
                barrier.Lane = passLane;
                if (const auto trIt = transitionsForPass.find(passName); trIt != transitionsForPass.end())
                    barrier.Transitions = trIt->second;
                plan.push_back(std::move(barrier));
            }

            // Pass command.
            SubmissionCommand passCmd;
            passCmd.CommandKind = SubmissionCommand::Kind::Pass;
            passCmd.NodeName = passName;
            passCmd.NodePointer = nodePtr;
            passCmd.WorkType = passWorkType;
            passCmd.Lane = passLane;
            plan.push_back(std::move(passCmd));

            // Signals are emitted after the producer body. The executor
            // submits this command-buffer segment only after it has staged all
            // signals in the command, so one producer fan-out still costs one
            // submission rather than one submit per resource edge.
            if (const auto signalIt = signalsByPass.find(passName); signalIt != signalsByPass.end())
            {
                SubmissionCommand signal;
                signal.CommandKind = SubmissionCommand::Kind::FenceSignal;
                signal.Lane = passLane;
                signal.FenceEdges.reserve(signalIt->second.size());
                for (const auto* edge : signalIt->second)
                    signal.FenceEdges.push_back(*edge);
                plan.push_back(std::move(signal));
            }
        }

        // Close any trailing open batch.
        if (currentBatch != std::numeric_limits<u32>::max())
        {
            SubmissionCommand end;
            end.CommandKind = SubmissionCommand::Kind::BatchEnd;
            end.BatchIndex = currentBatch;
            end.Lane = RenderGraph::QueueLane::Compute;
            if (const auto batchInfoIt = batchByIndex.find(currentBatch); batchInfoIt != batchByIndex.end())
            {
                end.SignalNodes = batchInfoIt->second->SignalNodes;
                end.OutputResources = batchInfoIt->second->OutputResources;
            }
            plan.push_back(std::move(end));
        }

        return plan;
    }
} // namespace OloEngine::RenderGraphSubmissionPlan
