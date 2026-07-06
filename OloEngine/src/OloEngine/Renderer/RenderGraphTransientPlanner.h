#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine::RenderGraphTransientPlanner
{
    // Transient-resource planner extracted from
    // `RenderGraph::RebuildTransientPlan` (+ its four descriptor helpers)
    // as part of the Phase 7 module split (2026-05-11). The planner computes
    // per-frame lifetimes for graph-declared transient resources, decides
    // whether each is allocatable, and assigns alias slots that let
    // non-overlapping transients share pool-allocated backing.
    //
    // The runtime `TransientPool` (`m_TransientPool`) that materialises the
    // assigned slots stays on the graph because it owns the GPU object cache;
    // this module is pure planning over descriptors + execution metadata.

    // ----------------------------------------------------------------
    // Pure descriptor helpers (no graph state — usable from any caller).
    // ----------------------------------------------------------------
    [[nodiscard]] auto BuildAliasGroup(const RGResourceDesc& desc) -> std::string;
    // Same identity as BuildAliasGroup but as a 64-bit hash for O(1) lookup
    // and cheap sort comparison. The string form remains available for the
    // public `TransientPlanEntry::AliasGroup` field and JSON dumps.
    [[nodiscard]] auto HashAliasGroup(const RGResourceDesc& desc) -> u64;
    [[nodiscard]] auto EstimateBytes(const RGResourceDesc& desc) -> u64;
    [[nodiscard]] auto IsAllocatable(const RGResourceDesc& desc) -> bool;
    [[nodiscard]] auto GetSkipReason(const RGResourceDesc& desc) -> std::string_view;

    // ----------------------------------------------------------------
    // Plan computation.
    // ----------------------------------------------------------------
    struct PlanInput
    {
        const std::unordered_map<std::string, RGResourceDesc>& TransientResourceDescs;
        std::span<const std::string> ExecutionOrder;
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        // Parent framebuffers whose lifetime a pass extends via an
        // attachment-view write, without a hazard-tracked access declaration
        // (RGBuilder::GetDeclaredLifetimeExtensions — see the comment in
        // RGBuilder::Write for why this is kept separate from
        // PassAccessDeclarations).
        const std::unordered_map<std::string, std::vector<std::string>>& PassLifetimeExtensions;
        std::function<bool(const std::string&)> IsPassReachable;
        std::function<bool(std::string_view)> IsExternallyBackedTransientResource;
    };

    [[nodiscard]] auto ComputePlan(const PlanInput& input) -> std::vector<RenderGraph::TransientPlanEntry>;
} // namespace OloEngine::RenderGraphTransientPlanner
