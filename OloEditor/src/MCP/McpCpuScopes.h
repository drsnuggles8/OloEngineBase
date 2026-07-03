#pragma once

// Pure JSON shaping for the olo_perf_cpu_scopes MCP tool (issue #519, first
// slice: expose PerformanceProfiler's per-scope CPU timings — every system in
// Scene.cpp wrapped in OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO — over MCP,
// read-only). The MCP handler in McpTools.cpp reads
// Application::Get().GetProfilerPreviousFrameData() inside a MarshalRead,
// pre-resolves it into the plain input structs below, and hands it here.
//
// OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO compile to a no-op in Distribution
// builds (see PerformanceProfiler.h), so an empty scope list is ambiguous:
// "nothing was timed this frame" vs "this build can't time anything at all".
// The handler resolves that ambiguity by passing `scopesCompiledIn` (a
// compile-time constant in McpTools.cpp, not something this pure function can
// know) so callers get an explicit "unavailable in this build" status instead
// of a silently empty list that looks broken.
//
// Keeping the shaping in a free function over engine-free input means it
// unit-tests directly against synthetic data — the test binary compiles this
// header but deliberately NOT McpTools.cpp. Mirrors the sibling pattern of
// McpPassTimings.h / McpFrameBreakdown.h.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::MCP::CpuScopes
{
    using Json = nlohmann::json;

    // One named scope's accumulated CPU time for the previous frame (mirrors
    // OloEngine::PerFrameData, kept engine-free for pure-header testability).
    struct ScopeEntry
    {
        std::string Name;
        f32 TimeMs = 0.0f;
        u32 Samples = 0;
    };

    // Millisecond values span sub-ms scopes to multi-ms frame roots — keep 3 decimals.
    [[nodiscard]] inline f64 Round3(f64 v)
    {
        return std::round(v * 1000.0) / 1000.0;
    }

    // scopesCompiledIn: whether OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO emit real
    // timers in this build (false in Distribution — the handler resolves this
    // from the same #if that guards the macros). When false, `scopes` is
    // expected to be empty and the result reports status "unavailable"
    // regardless of what was passed, rather than a misleadingly empty
    // "ok" / zero-scopes result.
    [[nodiscard]] inline Json BuildCpuScopes(const std::vector<ScopeEntry>& scopes, bool scopesCompiledIn, u32 limit)
    {
        Json o;

        if (!scopesCompiledIn)
        {
            o["status"] = "unavailable";
            o["note"] = "CPU scopes unavailable in this build (OLO_PERF_SCOPE is compiled out in Distribution). "
                        "Rebuild Debug or Release to collect per-scope CPU timings.";
            o["scopes"] = Json::array();
            o["totalTimeMs"] = 0.0;
            o["scopeCount"] = 0;
            return o;
        }

        std::vector<const ScopeEntry*> sorted;
        sorted.reserve(scopes.size());
        for (const auto& s : scopes)
            sorted.push_back(&s);
        std::sort(sorted.begin(), sorted.end(),
                  [](const ScopeEntry* a, const ScopeEntry* b)
                  { return a->TimeMs > b->TimeMs; });

        f64 totalTimeMs = 0.0;
        for (const auto& s : scopes)
            totalTimeMs += s.TimeMs;

        Json arr = Json::array();
        const sizet count = limit > 0 ? std::min<sizet>(sorted.size(), limit) : sorted.size();
        for (sizet i = 0; i < count; ++i)
        {
            arr.push_back(Json{ { "name", sorted[i]->Name },
                                { "timeMs", Round3(sorted[i]->TimeMs) },
                                { "samples", sorted[i]->Samples } });
        }

        o["status"] = scopes.empty() ? "ok_no_data" : "ok";
        o["note"] = scopes.empty()
                        ? "No scopes recorded last frame — OLO_PERF_SCOPE code paths may not have run, or the "
                          "engine hasn't completed a frame yet."
                        : "Per-scope CPU time accumulated across all invocations of that scope name last frame "
                          "(a scope hit multiple times per frame sums into one entry, see samples).";
        o["scopes"] = std::move(arr);
        o["totalTimeMs"] = Round3(totalTimeMs);
        o["scopeCount"] = static_cast<u64>(scopes.size());
        return o;
    }
} // namespace OloEngine::MCP::CpuScopes
