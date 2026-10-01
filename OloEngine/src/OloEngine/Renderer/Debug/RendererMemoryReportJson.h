#pragma once

#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"

#include <nlohmann/json.hpp>

// The one JSON form of RendererMemoryReport (issue #1342), shared by olo_memory_report and
// the benchmark export so the two can never describe the same report differently.
//
// Every byte field is named *Bytes and is an integer count of bytes. GPU and CPU are
// separate objects. A quantity with no measurement behind it is JSON null, never 0, and
// the row says why (unknownReason); residency on a backend that cannot observe it is the
// string "unknown", never a zero-byte heap.
namespace OloEngine
{
    [[nodiscard]] nlohmann::json RendererMemoryReportToJson(const RendererMemoryReport& report);

    // Just the totals, validity and backend — what a per-run benchmark record carries.
    [[nodiscard]] nlohmann::json RendererMemorySummaryToJson(const RendererMemoryReport& report);
} // namespace OloEngine
