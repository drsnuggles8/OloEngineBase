#pragma once

// Pure JSON shaping behind olo_virtual_shadow_map_stats. The live handler
// flattens VirtualShadowMap's CPU-cached, previous-frame statistics into this
// engine-free snapshot inside a main-thread marshal.

#include "MCP/McpStatsSnapshot.h"

namespace OloEngine::MCP::VirtualShadowMapStats
{
    using Json = nlohmann::json;

    struct Snapshot
    {
        StatsSnapshot::State State;
        u32 PhysicalResolution = 0;
        u32 PageSize = 0;
        u64 PhysicalPageCount = 0;
        u64 VRAMBytes = 0;
        u32 PagesRequested = 0;
        u32 PagesAllocated = 0;
        u32 PagesFailed = 0;
        u32 PagesDrawn = 0;
        u32 PagesResident = 0;
        u32 PagesFreed = 0;
        u32 DrawInstances = 0;
        u32 CullOverflows = 0;
        u32 LocalPagesResident = 0;
        u32 LocalPagesDrawn = 0;
    };

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        out["physicalPool"] = Json{ { "resolution", snapshot.PhysicalResolution },
                                    { "pageSize", snapshot.PageSize },
                                    { "pageCount", snapshot.PhysicalPageCount } };
        out["vramBytes"] = snapshot.VRAMBytes;
        out["statistics"] = Json{ { "pagesRequested", snapshot.PagesRequested },
                                  { "pagesAllocated", snapshot.PagesAllocated },
                                  { "pagesFailed", snapshot.PagesFailed },
                                  { "pagesDrawn", snapshot.PagesDrawn },
                                  { "pagesResident", snapshot.PagesResident },
                                  { "pagesFreed", snapshot.PagesFreed },
                                  { "drawInstances", snapshot.DrawInstances },
                                  { "cullOverflows", snapshot.CullOverflows },
                                  { "localPagesResident", snapshot.LocalPagesResident },
                                  { "localPagesDrawn", snapshot.LocalPagesDrawn } };
        return out;
    }
} // namespace OloEngine::MCP::VirtualShadowMapStats
