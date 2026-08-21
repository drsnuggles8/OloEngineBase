#pragma once

// Pure JSON shaping for the `olo_gpu_readback_stats` MCP tool (issue #721).
//
// The handler in McpToolsRender.cpp drains one frame off the GPU readback-stats
// channel inside a MarshalRead, flattens it into the engine-free `StatsSnapshot`
// below, and hands it here. Nothing in this header touches the renderer, so it
// unit-tests directly against synthetic data — the sibling pattern of
// McpPassTimings.h / McpFrameBreakdown.h, and the reason it is testable at all
// (a real handler needs MarshalRead, a game thread and GL).
//
// WHAT THIS TOOL IS FOR, and why it shapes the JSON the way it does. An agent
// asking "did that pass do what I think it did?" gets two useless answers from a
// naive dump: a wall of counters with no indication of freshness, and an
// overflow buried three keys deep. So:
//
//   * `overflows` is a TOP-LEVEL array and is populated first. It is the answer
//     to "is anything wrong", and it must not require reading the counters to
//     find.
//   * `frameIndex` / `latencyFrames` ride alongside every response. A counter
//     quoted without them cannot be distinguished from a counter that stopped
//     updating, and "the number looked plausible" is the exact failure this
//     channel exists to remove.
//   * `valid: false` is reported explicitly rather than as an empty counter
//     list. "Nothing has come back yet" and "everything counted zero" are
//     different facts and an agent will act differently on them.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace OloEngine::MCP::GpuReadbackStats
{
    using Json = nlohmann::json;

    struct CounterEntry
    {
        std::string Name;
        std::string Description;
        u32 Value = 0;
    };

    struct OverflowEntry
    {
        std::string Name;
        std::string Description;
    };

    // Engine-free mirror of one drained GPUReadbackStatsFrame plus the channel's
    // live state. Flattened by the handler so this header needs no renderer type.
    struct StatsSnapshot
    {
        bool Enabled = false;
        bool Valid = false; // a frame has actually come back
        u64 FrameIndex = 0;
        u32 LatencyFrames = 0;
        u32 RingSlots = 0;
        u32 SlotsInFlight = 0;
        std::vector<CounterEntry> Counters;
        std::vector<OverflowEntry> Overflows; // only the flags that fired
    };

    [[nodiscard]] inline Json BuildStatsReport(const StatsSnapshot& snapshot)
    {
        Json out;
        out["enabled"] = snapshot.Enabled;
        out["valid"] = snapshot.Valid;

        Json overflows = Json::array();
        for (const auto& entry : snapshot.Overflows)
        {
            overflows.push_back(Json{ { "name", entry.Name }, { "description", entry.Description } });
        }
        out["overflows"] = std::move(overflows);
        // A boolean beside the array, deliberately redundant. An agent that
        // branches on truthiness of a JSON array gets it wrong in several
        // languages, and this is the one field it must not get wrong.
        out["anyOverflow"] = !snapshot.Overflows.empty();

        out["frameIndex"] = snapshot.FrameIndex;
        out["latencyFrames"] = snapshot.LatencyFrames;
        out["ringSlots"] = snapshot.RingSlots;
        out["slotsInFlight"] = snapshot.SlotsInFlight;
        // The ring being full means captures are being SKIPPED, so the counters
        // are older than latencyFrames last managed to say. Named rather than
        // left for the reader to derive from two numbers.
        out["ringSaturated"] = snapshot.RingSlots > 0 && snapshot.SlotsInFlight >= snapshot.RingSlots;

        Json counters = Json::array();
        for (const auto& entry : snapshot.Counters)
        {
            counters.push_back(
                Json{ { "name", entry.Name }, { "description", entry.Description }, { "value", entry.Value } });
        }
        out["counters"] = std::move(counters);

        if (!snapshot.Enabled)
        {
            out["note"] = "The GPU readback-stats channel is disabled; counters are not being collected. "
                          "Enable RendererSettings::GPUReadbackStatsEnabled.";
        }
        else if (!snapshot.Valid)
        {
            out["note"] = "No frame has returned from the readback ring yet. The channel reads N frames late "
                          "by design; retry after a few frames have rendered.";
        }
        return out;
    }
} // namespace OloEngine::MCP::GpuReadbackStats
