#pragma once

// Pure JSON shaping behind olo_render_lod_stats. Renderer3D's classic-mesh LOD
// counters are cumulative from renderer initialization until ResetStats(), not
// a single-frame sample; the freshness envelope states that explicitly.

#include "MCP/McpStatsSnapshot.h"

#include <vector>

namespace OloEngine::MCP::RenderLODStats
{
    using Json = nlohmann::json;

    struct Snapshot
    {
        StatsSnapshot::State State;
        u32 LODSwitches = 0;
        std::vector<u32> ObjectsPerLODLevel;
    };

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        out["lodSwitches"] = snapshot.LODSwitches;
        out["objectsPerLODLevel"] = snapshot.ObjectsPerLODLevel;
        return out;
    }
} // namespace OloEngine::MCP::RenderLODStats
