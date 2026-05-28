#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphSubmissionPlan.h"

#include "OloEngine/Debug/Profiler.h"

#include <algorithm>
#include <limits>
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
            for (auto& [res, externalNode] : inputByResource)
                batch.InputResources.push_back({ res, externalNode });
            std::ranges::sort(batch.InputResources,
                              [](const BatchResourceDependency& a, const BatchResourceDependency& b)
                              { return a.ResourceName < b.ResourceName; });

            batch.OutputResources.reserve(outputByResource.size());
            for (auto& [res, externalNode] : outputByResource)
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

            // Memory barrier before this pass (if any).
            if (const auto barIt = barrierForPass.find(passName); barIt != barrierForPass.end())
            {
                SubmissionCommand barrier;
                barrier.CommandKind = SubmissionCommand::Kind::MemoryBarrier;
                barrier.Barriers = barIt->second;
                barrier.Lane = passLane;
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
