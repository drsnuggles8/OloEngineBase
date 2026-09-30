#pragma once

// Pure JSON shaping behind olo_streaming_stats (issue #1365).
//
// WHY THIS TOOL EXISTS. A region that is not loaded used to be silent: the
// streamer's only outside-visible numbers were a loaded count and a pending
// count. Once admission can DEFER or REJECT a request against a byte budget,
// "why is this region missing?" has an answer inside the process, and this tool
// is how it gets out: per region, the estimated size and the last admission
// outcome with its reason; in total, resident and pending bytes, and what
// admission, cancellation and eviction did.
//
// UNKNOWN IS NOT ZERO. Every byte total carries unknownCount next to knownBytes,
// and every per-region size says whether it is an estimate, an actual, or
// unknown (bytes: null). A reader summing knownBytes alone must check
// `complete` first.
//
// TWO BLOCKS. `sceneStreamer` is the active scene's region streamer; the
// editor only has one while it plays (or runs the editor streamer), so
// available:false is the normal state of an idle editor. `runtimeAssets` is the
// packed-asset async queue of a RuntimeAssetManager; the editor runs an
// EditorAssetManager, so it is available only in a runtime-style process.

#include "OloEngine/Asset/AssetByteSize.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace OloEngine::MCP::StreamingStats
{
    using Json = nlohmann::json;

    struct RegionRow
    {
        u64 Id = 0;
        std::string Name;
        std::string State;
        FAssetByteSize EstimatedSize;
        EStreamingAdmissionStatus AdmissionStatus = EStreamingAdmissionStatus::None;
        EStreamingAdmissionReason AdmissionReason = EStreamingAdmissionReason::None;
    };

    [[nodiscard]] inline const char* SourceName(EAssetByteSizeSource source)
    {
        switch (source)
        {
            case EAssetByteSizeSource::Estimate:
                return "estimate";
            case EAssetByteSizeSource::Actual:
                return "actual";
            case EAssetByteSizeSource::Unknown:
                break;
        }
        return "unknown";
    }

    [[nodiscard]] inline Json ByteSizeToJson(const FAssetByteSize& size)
    {
        Json j;
        j["source"] = SourceName(size.GetSource());
        if (const auto bytes = size.GetBytes())
            j["bytes"] = *bytes;
        else
            j["bytes"] = nullptr;
        return j;
    }

    [[nodiscard]] inline Json ByteTotalToJson(const FAssetByteTotal& total)
    {
        return Json{ { "knownBytes", total.KnownBytes },
                     { "actualBytes", total.ActualBytes },
                     { "estimateBytes", total.EstimateBytes },
                     { "count", total.Count },
                     { "unknownCount", total.UnknownCount },
                     { "complete", total.IsComplete() } };
    }

    // A budget of 0 is "no budget", which JSON says as null rather than a zero a
    // reader could compare against.
    [[nodiscard]] inline Json BudgetToJson(u64 bytes)
    {
        return bytes == 0 ? Json(nullptr) : Json(bytes);
    }

    [[nodiscard]] inline Json SceneStreamerToJson(const FSceneStreamingStats& stats, const std::vector<RegionRow>& regions)
    {
        Json j;
        j["available"] = true;
        j["loadedRegions"] = stats.LoadedRegions;
        j["maxLoadedRegions"] = stats.MaxLoadedRegions;
        j["pendingLoads"] = stats.PendingLoads;
        j["abandonedLoadsRunning"] = stats.AbandonedLoadsRunning;
        j["residentBytes"] = ByteTotalToJson(stats.ResidentBytes);
        j["pendingBytes"] = ByteTotalToJson(stats.PendingBytes);
        j["maxResidentBytes"] = BudgetToJson(stats.MaxResidentBytes);
        j["maxAdmittedBytesPerFrame"] = BudgetToJson(stats.MaxAdmittedBytesPerFrame);
        j["admittedBytesThisFrame"] = stats.AdmittedBytesThisFrame;
        j["admission"] = Json{ { "deferredRegions", stats.DeferredRegions },
                               { "rejectedRegions", stats.RejectedRegions },
                               { "deferredRequests", stats.DeferredRequests },
                               { "rejectedRequests", stats.RejectedRequests },
                               { "admittedUnknownSize", stats.AdmittedUnknownSize } };
        j["cancellation"] = Json{ { "cancelledBeforeStart", stats.CancelledBeforeStart },
                                  { "abandonedInFlight", stats.AbandonedInFlight },
                                  { "discardedCompleted", stats.DiscardedCompleted },
                                  { "abandonedResultsDropped", stats.AbandonedResultsDropped } };
        j["eviction"] = Json{ { "forCount", stats.EvictedForCount }, { "forBytes", stats.EvictedForBytes } };

        Json rows = Json::array();
        for (const RegionRow& row : regions)
        {
            rows.push_back(Json{ { "id", std::to_string(row.Id) },
                                 { "name", row.Name },
                                 { "state", row.State },
                                 { "estimatedSize", ByteSizeToJson(row.EstimatedSize) },
                                 { "admissionStatus", ToString(row.AdmissionStatus) },
                                 { "admissionReason", ToString(row.AdmissionReason) } });
        }
        j["regions"] = std::move(rows);
        return j;
    }

    [[nodiscard]] inline Json RuntimeAssetsToJson(const FRuntimeAssetStreamingReport& report)
    {
        const FRuntimeAssetLoadStats& loads = report.Loads;
        Json j;
        j["available"] = true;
        j["pendingCount"] = loads.PendingCount;
        j["completedUnretrievedCount"] = loads.CompletedUnretrievedCount;
        j["pendingBytes"] = ByteTotalToJson(loads.PendingBytes);
        j["residentBytes"] = ByteTotalToJson(report.Resident);
        j["abandonedRunningCount"] = loads.AbandonedRunningCount;
        j["cancellation"] = Json{ { "cancelledBeforeStart", loads.CancelledBeforeStart },
                                  { "abandonedInFlight", loads.AbandonedInFlight },
                                  { "discardedCompleted", loads.DiscardedCompleted },
                                  { "abandonedResultsDropped", loads.AbandonedResultsDropped } };
        j["rejectedWhileStopped"] = loads.RejectedWhileStopped;
        return j;
    }

    [[nodiscard]] inline Json Unavailable(const char* reason)
    {
        return Json{ { "available", false }, { "reason", reason } };
    }
} // namespace OloEngine::MCP::StreamingStats
