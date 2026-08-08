#pragma once

#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::RenderGraphBarrierPlanner
{
    // Backend-agnostic barrier-planning module.
    //
    // The planner consumes the compiled-frame access declarations and execution
    // order and emits a `PlannedBarrier` list, a per-pass barrier-flag map, and
    // a diagnostics list. It does not touch the GPU or any backend executor —
    // the result is a pure data structure the executor consumes via the
    // abstract `RGCommandContext::MemoryBarrier(flags)` entry point.
    //
    // Phase 7 module split (2026-05-11): this file isolates the barrier planner
    // from the `RenderGraph` god-class. The previous in-class
    // `RenderGraph::ComputeBarrierPlan` and `RenderGraph::GetResourceTransitions`
    // methods now delegate here.

    // ------------------------------------------------------------------------
    // Pure flag-resolution helpers (no state — usable from any caller).
    //
    // ADR 0011 §1.5: these two are the GL LOWERING — the MemoryBarrierFlags
    // they produce feed glMemoryBarrier and nothing else. The neutral truth a
    // transition carries is the RHI::Access pair below.
    // ------------------------------------------------------------------------
    [[nodiscard]] auto ResolveProducerBarrierFlags(RGWriteUsage usage) -> MemoryBarrierFlags;
    [[nodiscard]] auto ResolveConsumerBarrierFlags(RGReadUsage usage) -> MemoryBarrierFlags;

    // ------------------------------------------------------------------------
    // Builder-usage → unified-access mapping (ADR 0011 §1.5, Phase 5).
    //
    // RGWriteUsage / RGReadUsage stay the builder's declaration vocabulary;
    // the transition record is typed with RHI::Access so a write→write pair
    // is representable. Notes on the two deliberate collapses:
    //   - ShaderImage and ShaderStorage both map to Storage{Read,Write}: the
    //     image-vs-buffer distinction is recovered from the resource's kind
    //     at lowering time (only images have layouts; the GL flag split is
    //     preserved separately by the flag resolvers above).
    //   - RGWriteUsage::Clear maps to ClearAsLoadOp: its only producer site
    //     (OITPrepareRenderPass) is a load-op clear. An explicit
    //     vkCmdClearColorImage-style clear must be declared TransferDest
    //     (→ TransferWrite) instead — that is the §1.5 Clear split.
    // ------------------------------------------------------------------------
    [[nodiscard]] auto AccessForWriteUsage(RGWriteUsage usage) -> RHI::Access;
    [[nodiscard]] auto AccessForReadUsage(RGReadUsage usage) -> RHI::Access;

    // ------------------------------------------------------------------------
    // ComputeBarrierPlan inputs/outputs.
    // ------------------------------------------------------------------------
    struct PlanInput
    {
        std::span<const std::string> ExecutionOrder;
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        // Returns true when the named pass is reachable from the final pass
        // and therefore will actually execute this frame.
        std::function<bool(const std::string&)> IsPassReachable;
    };

    struct PlanResult
    {
        std::vector<RenderGraph::PlannedBarrier> PlannedBarriers;
        std::unordered_map<std::string, MemoryBarrierFlags> PassBarrierFlags;
        std::vector<RenderGraph::BarrierDiagnostic> Diagnostics;
    };

    [[nodiscard]] auto ComputePlan(const PlanInput& input) -> PlanResult;

    // ------------------------------------------------------------------------
    // BuildResourceTransitions inputs/output.
    //
    // Cross-lane sync annotation requires knowing each pass's work type
    // (Graphics / Compute / Copy). Callers provide a lookup functor so this
    // module stays free of `RenderGraph`-wide accessors.
    // ------------------------------------------------------------------------
    struct TransitionInput
    {
        std::span<const RenderGraph::PlannedBarrier> PlannedBarriers;
        std::span<const std::string> ExecutionOrder;
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        std::function<RenderGraphPassWorkType(const std::string&)> GetPassWorkType;
    };

    [[nodiscard]] auto BuildResourceTransitions(const TransitionInput& input) -> std::vector<RenderGraph::ResourceTransition>;
} // namespace OloEngine::RenderGraphBarrierPlanner
