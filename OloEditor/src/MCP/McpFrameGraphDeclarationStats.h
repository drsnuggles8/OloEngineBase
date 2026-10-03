#pragma once

// Pure JSON shaping for the `olo_frame_graph_declaration_stats` MCP tool
// (issue #607, gap logged from #1333).
//
// The handler in McpToolsResources.cpp reads Renderer3D::GetFrameGraphDeclarationStats()
// and the OLO_RG_VERIFY_DECLARATION_CACHE lever inside a MarshalRead and hands
// both here. Nothing below touches the renderer, so the shaping unit-tests
// against synthetic stats (McpFrameGraphDeclarationStatsTest).
//
// What the shaping has to get right:
//
//   * `staleCacheDetections` is only ever counted under verify mode. Without
//     it, 0 means "nobody checked", not "the cache is sound", so the report
//     carries `verifyMode` beside the count and a note saying so. The two
//     readings call for opposite conclusions about the cache.
//   * `anyStale` is a top-level boolean. A stale detection is the defect this
//     instrument exists to find (a declaration input missing from
//     FrameGraphDeclarationConfig), and it must not be buried in the counters.
//   * Means are null, not 0, when nothing was counted. A 0 µs mean compile is a
//     measurement; no compiles is the absence of one.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/FrameGraphDeclarationConfig.h"

#include <nlohmann/json.hpp>

namespace OloEngine::MCP::FrameGraphDeclaration
{
    using Json = nlohmann::json;

    [[nodiscard]] inline Json MeanOrNull(f64 total, u64 count)
    {
        if (count == 0)
            return nullptr;
        return total / static_cast<f64>(count);
    }

    [[nodiscard("this builds the response; it does not send it")]] inline Json
    BuildStatsReport(const FrameGraphDeclarationStats& stats, bool verifyMode)
    {
        Json out;
        out["verifyMode"] = verifyMode;
        out["anyStale"] = stats.StaleCacheDetections > 0;

        out["frames"] = stats.Frames;
        out["compiles"] = stats.Compiles;
        out["cacheHits"] = stats.CacheHits;
        out["redundantCompiles"] = stats.RedundantCompiles;
        out["verifiedHits"] = stats.VerifiedHits;
        out["staleCacheDetections"] = stats.StaleCacheDetections;

        // Of the frames that went through the cache at all. Null before the
        // first one, for the reason the means are.
        const u64 decided = stats.Compiles + stats.CacheHits;
        out["cacheHitRate"] = decided == 0 ? Json(nullptr)
                                           : Json(static_cast<f64>(stats.CacheHits) / static_cast<f64>(decided));

        Json timing;
        timing["compileMicrosTotal"] = stats.CompileMicrosTotal;
        timing["cacheHitMicrosTotal"] = stats.CacheHitMicrosTotal;
        timing["digestMicrosTotal"] = stats.DigestMicrosTotal;
        timing["lastCompileMicros"] = stats.LastCompileMicros;
        timing["lastCacheHitMicros"] = stats.LastCacheHitMicros;
        timing["meanCompileMicros"] = MeanOrNull(stats.CompileMicrosTotal, stats.Compiles);
        timing["meanCacheHitMicros"] = MeanOrNull(stats.CacheHitMicrosTotal, stats.CacheHits);
        out["timing"] = std::move(timing);

        out["lastCompileCause"] = stats.LastCompileCause;
        out["lastStaleCacheDetail"] = stats.LastStaleCacheDetail;

        if (!verifyMode)
        {
            out["note"] = "Verify mode is off, so staleCacheDetections and verifiedHits are not being counted: 0 means "
                          "nobody checked, not that the cache is sound. Set the OLO_RG_VERIFY_DECLARATION_CACHE "
                          "lever (olo_cvar_set) to rebuild and compare every cache hit; it costs a full rebuild per "
                          "frame.";
        }
        else if (stats.StaleCacheDetections > 0)
        {
            out["note"] = "The declaration cache served a stale plan: an input that changes the compiled frame "
                          "graph is missing from FrameGraphDeclarationConfig. lastStaleCacheDetail names the plan "
                          "entries that differed.";
        }
        return out;
    }
} // namespace OloEngine::MCP::FrameGraphDeclaration
