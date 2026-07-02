#pragma once

// Pure JSON shaping for the olo_perf_pass_timings MCP tool (issue #316: split
// whole-frame GPU time by render-graph pass — Shadow vs Scene vs GTAO vs Bloom
// vs ToneMap...). The MCP handler in McpTools.cpp gathers, inside a MarshalRead:
//   - per-pass GPU times from GPUPassTimerPool (always-on GL_TIMESTAMP pairs,
//     resolved 1-3 frames after issue),
//   - per-pass CPU times from the live RenderGraph's last execution timings,
//   - frame totals from RendererProfiler,
// pre-resolves them into the plain input structs below, and hands them here.
//
// Keeping the shaping in free functions over engine-free inputs means it
// unit-tests directly against synthetic data — the test binary compiles this
// header but deliberately NOT McpTools.cpp. Mirrors the sibling pattern of
// McpFrameBreakdown.h / McpRenderExplain.h.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::MCP::PassTimings
{
    using Json = nlohmann::json;

    // One render-graph pass's GPU time (execution order preserved).
    struct GpuPassEntry
    {
        std::string Name;
        f64 GpuMs = 0.0;
    };

    // One render-graph pass's CPU (submission/dispatch) time.
    struct CpuPassEntry
    {
        std::string Name;
        f64 CpuMs = 0.0;
    };

    struct FrameTotals
    {
        f64 FrameTimeMs = 0.0;
        f64 CpuMs = 0.0;
        f64 GpuMs = 0.0;     // whole-frame GPU time (timestamp span)
        f64 GpuWaitMs = 0.0; // CPU time blocked on the frame fence
        // How many frames old the resolved GPU numbers are (0 = nothing
        // resolved yet). GPU results always lag 1-3 frames behind the CPU
        // numbers; transient name mismatches between the two lists are normal.
        u64 GpuResultsAgeFrames = 0;
    };

    // Millisecond values are sub-ms for many passes — keep 3 decimals.
    [[nodiscard]] inline f64 Round3(f64 v)
    {
        return std::round(v * 1000.0) / 1000.0;
    }

    // Join the GPU list (primary, execution order) with CPU times by pass name;
    // CPU-only passes (not GPU-timed this frame — pool overflow or transient
    // topology change between the resolved GPU frame and the current CPU frame)
    // are appended with gpuMs 0.
    [[nodiscard]] inline Json BuildPassTimings(const std::vector<GpuPassEntry>& gpuPasses,
                                               const std::vector<CpuPassEntry>& cpuPasses,
                                               const FrameTotals& totals)
    {
        Json passes = Json::array();
        f64 passGpuTotal = 0.0;

        std::vector<bool> cpuUsed(cpuPasses.size(), false);
        const auto findCpuMs = [&](const std::string& name) -> f64
        {
            for (sizet i = 0; i < cpuPasses.size(); ++i)
            {
                if (!cpuUsed[i] && cpuPasses[i].Name == name)
                {
                    cpuUsed[i] = true;
                    return cpuPasses[i].CpuMs;
                }
            }
            return 0.0;
        };

        for (const auto& gpuPass : gpuPasses)
        {
            passGpuTotal += gpuPass.GpuMs;
            passes.push_back(Json{ { "pass", gpuPass.Name },
                                   { "gpuMs", Round3(gpuPass.GpuMs) },
                                   { "cpuMs", Round3(findCpuMs(gpuPass.Name)) } });
        }

        for (sizet i = 0; i < cpuPasses.size(); ++i)
        {
            if (cpuUsed[i])
                continue;
            passes.push_back(Json{ { "pass", cpuPasses[i].Name },
                                   { "gpuMs", 0.0 },
                                   { "cpuMs", Round3(cpuPasses[i].CpuMs) } });
        }

        Json o;
        o["frame"] = Json{ { "frameTimeMs", Round3(totals.FrameTimeMs) },
                           { "cpuMs", Round3(totals.CpuMs) },
                           { "gpuMs", Round3(totals.GpuMs) },
                           { "gpuWaitMs", Round3(totals.GpuWaitMs) } };
        o["passes"] = std::move(passes);
        o["passGpuTotalMs"] = Round3(passGpuTotal);
        // GPU time inside the frame span but between/outside timed passes
        // (barriers, transient materialization, HZB rebuild, capture readbacks).
        // Negative values are clamped: pass spans can overlap the frame span's
        // edges by a timestamp tick.
        o["unattributedGpuMs"] = Round3(totals.GpuMs > passGpuTotal ? totals.GpuMs - passGpuTotal : 0.0);
        o["gpuResultsAgeFrames"] = totals.GpuResultsAgeFrames;
        return o;
    }
} // namespace OloEngine::MCP::PassTimings
