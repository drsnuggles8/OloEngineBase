#pragma once

#include "OloEngine/Core/CpuScopeRing.h"

#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace OloEngine::MCP::CpuScopeRecording
{
    // Recording remains a bounded byte copy. Drop only an incomplete trailing
    // UTF-8 character here, off the recording thread, before strict JSON output.
    [[nodiscard]] inline std::string_view ExportName(const CpuScopeRing::Event& event)
    {
        std::string_view name(event.Name.data());
        if (!event.NameTruncated || name.empty())
            return name;
        sizet start = name.size() - 1;
        while (start > 0 && (static_cast<u8>(name[start]) & 0xC0u) == 0x80u)
            --start;
        const auto lead = static_cast<u8>(name[start]);
        const sizet width = lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3
                                            : lead >= 0xC0u   ? 2
                                                              : 1;
        return name.size() - start < width ? name.substr(0, start) : name;
    }

    // This is the real recorder snapshot, not a second synthetic input model.
    [[nodiscard]] inline nlohmann::json ToJson(const CpuScopeRing::Snapshot& capture)
    {
        using Json = nlohmann::json;
        Json out{ { "armed", capture.Armed }, { "frameCapacity", capture.FrameCapacity }, { "eventsPerFrame", capture.EventsPerFrame }, { "framesWritten", capture.FramesWritten }, { "framesOverwritten", capture.FramesOverwritten }, { "lateCompletions", capture.LateCompletions }, { "partialFramesDiscarded", capture.PartialFramesDiscarded }, { "retainedBytes", capture.RetainedBytes }, { "frames", Json::array() } };
        for (const auto& frame : capture.Frames)
        {
            Json scopes = Json::array();
            for (u32 i = 0; i < frame.EventCount; ++i)
            {
                const auto& event = capture.Events[static_cast<i32>(frame.EventOffset + i)];
                scopes.push_back(Json{ { "name", ExportName(event) }, { "nameTruncated", event.NameTruncated }, { "depth", event.Depth }, { "thread", event.Thread }, { "startMs", event.StartMs }, { "durationMs", event.Complete ? Json(event.DurationMs) : Json(nullptr) }, { "complete", event.Complete } });
            }
            out["frames"].push_back(Json{ { "frameId", frame.Id }, { "frameTimeMs", frame.ElapsedMs }, { "rendererCpuMs", frame.Render.RendererFrameAdvanced ? Json(frame.Render.RendererCpuMs) : Json(nullptr) }, { "rendererFrameId", frame.Render.RendererFrameId }, { "rendererFrameAdvanced", frame.Render.RendererFrameAdvanced }, { "gpuMs", frame.Render.GpuStatus == GpuTimingStatus::Valid ? Json(frame.Render.GpuMs) : Json(nullptr) }, { "gpuStatus", ToString(frame.Render.GpuStatus) }, { "gpuMeasurementFrameId", frame.Render.GpuFrameId }, { "gpuIssueFrameId", frame.Render.GpuIssueFrameId }, { "gpuFrameAdvanced", frame.Render.GpuFrameAdvanced }, { "sameFrameGpuMs", frame.SameFrameGpu.IsValid() ? Json(frame.SameFrameGpu.GpuMs) : Json(nullptr) }, { "sameFrameGpuStatus", ToString(frame.SameFrameGpu.Status) }, { "droppedEvents", frame.DroppedEvents }, { "scopes", std::move(scopes) } });
        }
        return out;
    }
} // namespace OloEngine::MCP::CpuScopeRecording
