#pragma once

#include "OloEngine/Core/CpuScopeRing.h"

#include <nlohmann/json.hpp>
#include <utility>

namespace OloEngine::MCP::CpuScopeRecording
{
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
                scopes.push_back(Json{ { "name", event.Name.data() }, { "nameTruncated", event.NameTruncated }, { "depth", event.Depth }, { "thread", event.Thread }, { "startMs", event.StartMs }, { "durationMs", event.Complete ? Json(event.DurationMs) : Json(nullptr) }, { "complete", event.Complete } });
            }
            out["frames"].push_back(Json{ { "frameId", frame.Id }, { "frameTimeMs", frame.ElapsedMs }, { "rendererCpuMs", frame.Render.RendererFrameAdvanced ? Json(frame.Render.RendererCpuMs) : Json(nullptr) }, { "rendererFrameId", frame.Render.RendererFrameId }, { "rendererFrameAdvanced", frame.Render.RendererFrameAdvanced }, { "gpuMs", frame.Render.GpuStatus == GpuTimingStatus::Valid ? Json(frame.Render.GpuMs) : Json(nullptr) }, { "gpuStatus", ToString(frame.Render.GpuStatus) }, { "gpuMeasurementFrameId", frame.Render.GpuFrameId }, { "gpuIssueFrameId", frame.Render.GpuIssueFrameId }, { "gpuFrameAdvanced", frame.Render.GpuFrameAdvanced }, { "sameFrameGpuMs", frame.SameFrameGpu.IsValid() ? Json(frame.SameFrameGpu.GpuMs) : Json(nullptr) }, { "sameFrameGpuStatus", ToString(frame.SameFrameGpu.Status) }, { "droppedEvents", frame.DroppedEvents }, { "scopes", std::move(scopes) } });
        }
        return out;
    }
} // namespace OloEngine::MCP::CpuScopeRecording
