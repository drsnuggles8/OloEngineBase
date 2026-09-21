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
    // as part of the module split (2026-05-11). This module turns the
    // compiled execution order + barrier plan + per-pass work-type / async
    // candidate metadata into a backend-agnostic submission IR
    // (`TArray64<SubmissionCommand>`) that the executor consumes verbatim.
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
        std::span<const FString> ExecutionOrder;
        const RGTransparentStringMap<TArray64<FString>>& Dependencies;
        const RGTransparentStringMap<TArray64<RGAccessDeclaration>>& PassAccessDeclarations;
        std::function<bool(std::string_view)> IsGraphEntryAsyncComputeCandidate;
    };

    [[nodiscard]] auto ComputeBatches(const BatchesInput& input) -> TArray64<RenderGraph::AsyncComputeBatch>;

    struct PlanInput
    {
        std::span<const FString> ExecutionOrder;
        // Incoming graph edges (consumer -> producers). Split submission is
        // derived from this complete ordering graph; resource transitions only
        // annotate those edges with the resources that caused them.
        const RGTransparentStringMap<TArray64<FString>>& Dependencies;
        std::span<const RenderGraph::PlannedBarrier> PlannedBarriers;
        // ADR 0011 §1.5: the per-resource transition records for
        // the same barrier plan. Attached (deduplicated) to each emitted
        // MemoryBarrier command so an explicit-barrier backend can lower
        // per-resource layout transitions without re-querying the graph.
        // May be empty (headless plan-shape tests) — GL execution never
        // reads them.
        std::span<const RenderGraph::ResourceTransition> Transitions;
        std::span<const RenderGraph::AsyncComputeBatch> Batches;
        // False under OLO_RENDERGRAPH_SEQUENTIAL. Normal MemoryBarrier
        // commands are always emitted; this only removes the additional
        // cross-submission signal/wait scheduling IR.
        bool EnableSplitBarriers = true;
        std::function<RenderGraphPassWorkType(std::string_view)> GetPassWorkType;
        // Returns the node pointer for the named pass, or nullptr if the
        // pass is unknown / external.
        std::function<RenderGraphNode*(std::string_view)> ResolveNodePointer;
        // Compiled reachability, when building a real graph. A culled member
        // must not make an otherwise valid recording group decline at runtime.
        std::function<bool(std::string_view)> IsPassReachable;
    };

    [[nodiscard]] auto BuildPlan(const PlanInput& input) -> TArray64<RenderGraph::SubmissionCommand>;
} // namespace OloEngine::RenderGraphSubmissionPlan
