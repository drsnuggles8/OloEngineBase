#pragma once

#include "OloEngine/Renderer/RGBuilder.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::RenderGraphReachability
{
    // Backward reachability scan extracted from `RenderGraph::ComputeReachability`
    // as part of the Phase 7 module split (2026-05-11). The pure-BFS portion
    // — building the resource-writer map, seeding the worklist from the final
    // pass + extract / history / external-sink roots, then expanding through
    // dependency edges and Read→Writer chains — lives here. The mutating
    // tail (refreshing temporal/external-sink contracts, walking insertion
    // order to fold side-effecting passes back into the reachable set, and
    // emitting the culled-pass digest log) stays on the graph because it
    // touches state outside the reachability scope.

    struct ScanInput
    {
        // When false, all passes are considered reachable — ad-hoc / unit-test
        // graphs without an explicit final pass keep their no-final
        // execution semantics.
        bool HasExplicitFinalPass = false;

        // The graph's designated final pass. Reachability roots from here.
        std::string_view FinalPassName;

        // Iteration order for building the writer map (matches the canonical
        // insertion order). Each entry must be a registered graph-entry name.
        std::span<const std::string> InsertionOrder;

        // Per-pass setup-time access declarations.
        const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& PassAccessDeclarations;

        // Explicit ordering edges (consumer → list of producer pass names).
        const std::unordered_map<std::string, std::vector<std::string>>& Dependencies;

        // Additional reachability roots derived from extraction / temporal-
        // history / external-sink contracts. Each entry is a resource name
        // whose writers must remain reachable. Empty entries are ignored.
        std::span<const std::string> ExtractedResourceNames;
    };

    [[nodiscard]] auto ComputeReachableSet(const ScanInput& input) -> std::unordered_set<std::string>;
} // namespace OloEngine::RenderGraphReachability
