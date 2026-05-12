#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine::RenderGraphResourceRegistry
{
    // Resource-registry builder extracted from
    // `RenderGraph::EnsureResourceRegistryBuilt` as part of the Phase 7 module
    // split (2026-05-11). This module owns the *pure* registry build (Phase A):
    // turning descriptor maps + per-pass access declarations into the canonical
    // `ResourceInfo` table, recording producer/consumer pass lists, and
    // emitting kind-mismatch diagnostics.
    //
    // The typed-handle slot reconciliation (Phase B — handle slot generations,
    // free-index lists, physical-resource arrays) stays on the graph because
    // it mutates handle allocators that other graph subsystems hold pointers
    // into.

    struct BuildInput
    {
        const std::unordered_map<std::string, RGResourceDesc>& ImportedResources;
        const std::unordered_map<std::string, RGResourceDesc>& TransientResourceDescs;
        const std::unordered_map<std::string, RGResourceDesc>& TextureViewResourceDescs;
        std::span<const std::string> InsertionOrder;
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;
        std::function<bool(std::string_view)> IsExternallyBackedTransientResource;
    };

    struct BuildResult
    {
        std::unordered_map<std::string, RenderGraph::ResourceInfo> Registry;
        // `Sorted` is the canonical execution-order view used by downstream
        // stages (hazard validator, transient planner). Sort key is resource
        // name (lexicographic) so the order is deterministic across rebuilds.
        std::vector<RenderGraph::ResourceInfo> Sorted;
        std::vector<RenderGraph::Hazard> Diagnostics;
    };

    [[nodiscard]] auto Build(const BuildInput& input) -> BuildResult;
} // namespace OloEngine::RenderGraphResourceRegistry
