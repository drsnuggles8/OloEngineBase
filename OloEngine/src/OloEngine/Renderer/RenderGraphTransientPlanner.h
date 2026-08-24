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
    // as part of the module split (2026-05-11). The planner computes
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
        // WriteNewVersion renames: versioned name → source resource name
        // (RenderGraph::m_VersionAliasTargets). A version is dependency
        // bookkeeping over the SAME physical resource, so its accesses fold
        // into the source's lifetime (following the chain to the base) and
        // the version itself is never allocated — before this, every version
        // got its own pool object, and RMW seams where the producer rendered
        // via the base handle left consumers reading a never-written orphan
        // (the one-frame black-square artifact on transient-plan rebuilds).
        const std::unordered_map<std::string, std::string>& VersionAliasTargets;
        std::function<bool(const std::string&)> IsPassReachable;
        std::function<bool(std::string_view)> IsExternallyBackedTransientResource;
        // Resources copied out AFTER the last pass has executed — today that is
        // the temporal-history sinks (RenderGraph::FlushExtractions). Their
        // lifetime does NOT end at their last pass access: the copy still has to
        // read them, so the alias-slot assigner must not hand their backing to a
        // later same-descriptor transient. Without this a pass that both writes
        // and last-reads its own history source inside one Execute — which is
        // every three-draw resolve — ends the frame copying whatever transient
        // happened to reuse the slot, and the temporal accumulation silently
        // becomes a no-op. Issue #902; see
        // docs/agent-rules/render-graph-transient-aliasing.md.
        std::function<bool(std::string_view)> IsExtractedAfterExecution;
    };

    [[nodiscard]] auto ComputePlan(const PlanInput& input) -> std::vector<RenderGraph::TransientPlanEntry>;
} // namespace OloEngine::RenderGraphTransientPlanner
