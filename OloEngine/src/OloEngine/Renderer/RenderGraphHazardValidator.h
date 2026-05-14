#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::RenderGraphHazardValidator
{
    // Resource-hazard validator extracted from `RenderGraph::ValidateResourceHazardsInternal`
    // as part of the Phase 7 module split (2026-05-11). The validator runs after
    // the resource registry and topological order are ready; it checks:
    //   - registry-stage diagnostics (e.g. kind mismatches) are still relevant
    //     for reachable passes.
    //   - same-pass overlapping read/write without an explicit feedback
    //     declaration (Feedback hazards).
    //   - imported resources that are produced & consumed in-graph but have no
    //     valid backing object (lifetime misuse).
    //   - cross-pass RAW / WAW / WAR violations not covered by the explicit
    //     dependency closure.
    //
    // The caller is responsible for keeping the registry and dependency graph
    // up to date and for surfacing cycle diagnostics; the module never mutates
    // graph state.

    struct ValidatorInput
    {
        // Predicates injected by the graph.
        std::function<bool(const std::string&)> IsPassReachable;
        std::function<u32(RGTextureHandle)> ResolveTexture;
        std::function<u32(RGBufferHandle)> ResolveBuffer;
        std::function<Ref<Framebuffer>(RGFramebufferHandle)> ResolveFramebuffer;

        // Topology (already up to date — caller has run UpdateDependencyGraph
        // and surfaced any cycle diagnostic).
        std::span<const std::string> ExecutionOrder;
        const std::unordered_map<std::string, std::vector<std::string>>& Dependencies;

        // Setup-time access + feedback declarations.
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        const std::unordered_map<std::string, std::vector<RGFeedbackDeclaration>>& PassFeedbackDeclarations;

        // Registry stage outputs.
        std::span<const RenderGraph::Hazard> RegistryDiagnostics;
        std::span<const RenderGraph::ResourceInfo> RegisteredResources;
    };

    [[nodiscard]] auto Validate(const ValidatorInput& input) -> std::vector<RenderGraph::Hazard>;
} // namespace OloEngine::RenderGraphHazardValidator
