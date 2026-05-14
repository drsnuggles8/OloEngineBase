#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::RenderGraphSubmissionPlan
{
    // Submission-plan / async-compute scheduler module extracted from
    // `RenderGraph::GetAsyncComputeBatches` + `RenderGraph::GetSubmissionPlan`
    // as part of the Phase 7 split (2026-05-11). This module turns the
    // compiled execution order + barrier plan + per-pass work-type / async
    // candidate metadata into a backend-agnostic submission IR
    // (`std::vector<SubmissionCommand>`) that the executor consumes verbatim.
    //
    // Two public surfaces:
    //   1. `ComputeBatches` — groups consecutive AsyncComputeCandidate passes
    //      into batches and derives the batch's wait/signal nodes plus the
    //      input/output resource dependencies. Used by the dump/diagnostic
    //      paths as well as the submission-plan builder.
    //   2. `BuildPlan` — walks the execution order, interleaves batch
    //      begin/end commands and memory-barrier commands with pass
    //      commands.

    struct BatchesInput
    {
        std::span<const std::string> ExecutionOrder;
        const std::unordered_map<std::string, std::vector<std::string>>& Dependencies;
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        std::function<bool(std::string_view)> IsGraphEntryAsyncComputeCandidate;
    };

    [[nodiscard]] auto ComputeBatches(const BatchesInput& input) -> std::vector<RenderGraph::AsyncComputeBatch>;

    struct PlanInput
    {
        std::span<const std::string> ExecutionOrder;
        std::span<const RenderGraph::PlannedBarrier> PlannedBarriers;
        std::span<const RenderGraph::AsyncComputeBatch> Batches;
        std::function<RenderGraphPassWorkType(const std::string&)> GetPassWorkType;
        // Returns the node pointer for the named pass, or nullptr if the
        // pass is unknown / external.
        std::function<RenderGraphNode*(const std::string&)> ResolveNodePointer;
    };

    [[nodiscard]] auto BuildPlan(const PlanInput& input) -> std::vector<RenderGraph::SubmissionCommand>;
} // namespace OloEngine::RenderGraphSubmissionPlan
