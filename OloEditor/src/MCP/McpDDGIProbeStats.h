#pragma once

// Pure JSON shaping behind olo_ddgi_probe_stats. The live handler performs the
// pass's deliberately blocking diagnostics readback once, then flattens the
// cached counters, bounce coverage and active cascade lattices here.

#include "MCP/McpStatsSnapshot.h"

#include <array>
#include <optional>
#include <vector>

namespace OloEngine::MCP::DDGIProbeStats
{
    using Json = nlohmann::json;

    struct Cascade
    {
        std::array<f32, 3> Origin{};
        std::array<f32, 3> Spacing{};
        std::array<i32, 3> LatticeMin{};
        std::array<i32, 3> Dimensions{};
    };

    struct Snapshot
    {
        StatsSnapshot::State State;
        u32 TotalProbes = 0;
        u32 LiveProbes = 0;
        u32 ActiveProbes = 0;
        u32 RelitProbes = 0;
        u32 CapturedProbes = 0;
        u32 BlendedProbes = 0;
        u32 UncapturedLive = 0;
        // nullopt means no active probe had a bounce hit to measure. Zero is a
        // legitimate measured coverage and must remain numeric.
        std::optional<f32> BounceCoverage;
        std::vector<Cascade> Cascades;
    };

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        out["totalProbes"] = snapshot.TotalProbes;
        out["statistics"] = Json{ { "liveProbes", snapshot.LiveProbes },
                                  { "activeProbes", snapshot.ActiveProbes },
                                  { "relitProbes", snapshot.RelitProbes },
                                  { "capturedProbes", snapshot.CapturedProbes },
                                  { "blendedProbes", snapshot.BlendedProbes },
                                  { "uncapturedLive", snapshot.UncapturedLive } };
        out["bounceCoverage"] = snapshot.BounceCoverage.has_value() ? Json(*snapshot.BounceCoverage) : Json(nullptr);

        Json cascades = Json::array();
        const sizet cascadeCount = snapshot.Cascades.size();
        for (sizet level = 0; level < cascadeCount; ++level)
        {
            const Cascade& cascade = snapshot.Cascades[level];
            cascades.push_back(Json{ { "level", level },
                                     { "origin", cascade.Origin },
                                     { "spacing", cascade.Spacing },
                                     { "latticeMin", cascade.LatticeMin },
                                     { "dimensions", cascade.Dimensions } });
        }
        out["cascades"] = std::move(cascades);
        return out;
    }
} // namespace OloEngine::MCP::DDGIProbeStats
