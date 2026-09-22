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
    // `RenderGraph::EnsureResourceRegistryBuilt` as part of the module
    // split (2026-05-11). This module owns the *pure* registry build:
    // turning descriptor maps + per-pass access declarations into the canonical
    // `ResourceInfo` table, recording producer/consumer pass lists, and
    // emitting kind-mismatch diagnostics.
    //
    // The typed-handle slot reconciliation (handle slot generations,
    // free-index lists, physical-resource arrays) stays on the graph because
    // it mutates handle allocators that other graph subsystems hold pointers
    // into.

    struct BuildInput
    {
        const RGTransparentStringMap<RGResourceDesc>& ImportedResources;
        const RGTransparentStringMap<RGResourceDesc>& TransientResourceDescs;
        const RGTransparentStringMap<RGResourceDesc>& TextureViewResourceDescs;
        std::span<const FString> InsertionOrder;
        const RGTransparentStringMap<TArray64<RGAccessDeclaration>>& PassAccessDeclarations;
        std::function<bool(std::string_view)> IsExternallyBackedTransientResource;
    };

    struct BuildResult
    {
        RGTransparentStringMap<RenderGraph::ResourceInfo> Registry;
        // `Sorted` is the canonical execution-order view used by downstream
        // stages (hazard validator, transient planner). Sort key is resource
        // name (lexicographic) so the order is deterministic across rebuilds.
        TArray64<RenderGraph::ResourceInfo> Sorted;
        TArray64<RenderGraph::Hazard> Diagnostics;
    };

    [[nodiscard]] auto Build(const BuildInput& input) -> BuildResult;
} // namespace OloEngine::RenderGraphResourceRegistry
