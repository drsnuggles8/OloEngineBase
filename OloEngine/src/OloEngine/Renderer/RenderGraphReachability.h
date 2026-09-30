#pragma once

#include "OloEngine/Renderer/RGBuilder.h"

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine::RenderGraphReachability
{
    // Backward reachability scan extracted from `RenderGraph::ComputeReachability`
    // as part of the module split (2026-05-11). The pure-BFS portion
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
        std::span<const FString> InsertionOrder;

        // Per-pass setup-time access declarations.
        const RGTransparentStringMap<TArray64<RGAccessDeclaration>>& PassAccessDeclarations;

        // Explicit ordering edges (consumer → list of producer pass names).
        const RGTransparentStringMap<TArray64<FString>>& Dependencies;

        // Additional reachability roots derived from extraction / temporal-
        // history / external-sink contracts. Each entry is a resource name
        // whose writers must remain reachable. Empty entries are ignored.
        std::span<const FString> ExtractedResourceNames;

        // Passes that are roots in their own right: side-effecting passes
        // (Present, NeverCull, readback). Seeding them, rather than folding
        // them back in after the scan, keeps what THEY read alive too — a
        // kept readback whose producer was culled reads a stale resource.
        std::span<const FString> SeedPasses;

        // Edges in Dependencies that only order two passes (write after read,
        // previous-frame read before an in-place rebuild). They are not
        // walked: the later pass does not consume the earlier one.
        const std::set<std::pair<std::string, std::string>>* OrderingOnlyEdges = nullptr; // (before, after)
    };

    [[nodiscard]] auto ComputeReachableSet(const ScanInput& input) -> RGTransparentStringSet;
} // namespace OloEngine::RenderGraphReachability
