#include "OloEnginePCH.h"
#include "MCP/McpCapturePath.h"
#include "MCP/McpCpuScopeRing.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpToolsCommon.h"
#include "OloEngine/Core/Application.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace OloEngine::MCP
{
    namespace
    {
        ToolResult HandleCpuScopeRing(IAutomationHost& host, const Json& args)
        {
#if !defined(OLO_DEBUG) && !defined(OLO_RELEASE)
            (void)host;
            (void)args;
            return ToolResult::Structured(Json{ { "status", "unavailable" },
                                                { "note", "CPU scopes are compiled out in Distribution; use Debug or Release." } });
#else
            const std::string action = args.value("action", std::string{ "status" });
            const u32 frames = args.value("frames", 512u);
            const u32 events = args.value("eventsPerFrame", 1024u);
            const f64 threshold = args.value("thresholdMs", 0.0);
            const u32 context = args.value("contextFrames", 0u);
            if (!std::isfinite(threshold) || threshold < 0)
                return ToolResult::Error("thresholdMs must be finite and nonnegative.");

            std::filesystem::path relative;
            std::filesystem::path absolute;
            const bool wantsFile = args.contains("path");
            if (wantsFile)
            {
                if (action != "dump")
                    return ToolResult::Error("path is only valid for action dump.");
                if (const auto error = CapturePath::Resolve(args.at("path").get<std::string>(), relative, absolute, ".json"))
                    return ToolResult::Error(*error);
            }

            // Only the bounded snapshot copy runs on the game thread. JSON
            // encoding and disk I/O stay on this handler thread. Shared ownership
            // also keeps the output alive if a queued read times out.
            auto capture = std::make_shared<CpuScopeRing::Snapshot>();
            const Json result = host.MarshalRead([capture, action, frames, events, threshold, context]() -> Json
                                                 {
                Application* app = Application::TryGet();
                if (!app)
                    return Json{ { "error", "Application is not running." } };
                auto& ring = app->GetPerformanceProfiler()->GetCpuScopeRing();
                if (action == "arm")
                    ring.Arm(frames, events);
                else if (action == "disarm")
                    ring.Disarm();
                else if (action != "dump" && action != "status")
                    return Json{ { "error", "action must be arm, disarm, dump or status." } };
                *capture = ring.Capture(threshold, context, action == "dump");
                return Json{ { "ok", true } }; });
            if (result.contains("error"))
                return ToolResult::Error(result.at("error").get<std::string>());
            Json out = CpuScopeRecording::ToJson(*capture);
            out["status"] = "ok";
            if (wantsFile)
            {
                std::ofstream file(absolute, std::ios::binary | std::ios::trunc);
                file << out.dump(1);
                file.close();
                if (!file)
                    return ToolResult::Error("Could not write the CPU scope capture under " + CapturePath::RootDisplay());
                out["capturedFrames"] = capture->Frames.Num();
                out["frames"] = Json::array();
                out["path"] = relative.generic_string();
            }
            return ToolResult::Structured(out);
#endif
        }
    } // namespace

    void RegisterCpuScopeRingTools(AutomationRegistry& registry)
    {
        ToolDef tool;
        tool.Name = "olo_perf_cpu_scope_ring";
        tool.Toolset = "perf";
        tool.Title = "Per-frame CPU scope recording";
        // Arm replaces a recording and a file dump may overwrite a capture.
        tool.Annotations = DestructiveMutatingAnnotations();
        tool.Description =
            "Arm a bounded in-process recorder for every windowed application frame's OLO_PERF_SCOPE events, "
            "disarm to freeze completed frames, inspect status, or dump. Arm allocates and touches storage "
            "once; recording reuses it without per-frame allocation. Frame/event loss and incomplete scopes "
            "are explicit. thresholdMs selects whole-loop elapsed frame times plus contextFrames neighbours. "
            "Scopes carry owned names, depth, thread, start and duration. Renderer CPU and delayed GPU "
            "measurements have their own frame IDs; rendererCpuMs is null on skipped render frames. "
            "Dump inline or as JSON under assets/mcp-captures with path. JSON encoding and disk I/O run "
            "off the game thread; dumping itself copies data at a frame boundary and is not part of "
            "an unperturbed measurement window. New arm discards the previous recording. Distribution "
            "reports unavailable because its scope macros are compiled out.";
        tool.InputSchema = Schema::Object()
                               .Prop("action", Schema::String().Enum({ "arm", "disarm", "dump", "status" }))
                               .Prop("frames", Schema::Int().Min(2).Max(2048).Desc("Frame slots (default 512)."))
                               .Prop("eventsPerFrame", Schema::Int().Min(1).Max(4096).Desc("Scope slots per frame (default 1024). Total storage cannot exceed 256 MiB."))
                               .Prop("thresholdMs", Schema::Number().Min(0).Desc("Dump frames at least this whole-loop elapsed time (default 0)."))
                               .Prop("contextFrames", Schema::Int().Min(0).Max(2048))
                               .Prop("path", Schema::String().Desc("Dump JSON filename under assets/mcp-captures; absolute paths and traversal are refused."))
                               .NoAdditional();
        tool.OutputSchema = Schema::Object()
                                .Prop("status", Schema::String().Enum({ "ok", "unavailable" }))
                                .Prop("note", Schema::String())
                                .Prop("armed", Schema::Bool())
                                .Prop("frameCapacity", Schema::Int().Min(0))
                                .Prop("eventsPerFrame", Schema::Int().Min(0))
                                .Prop("framesWritten", Schema::Int().Min(0))
                                .Prop("framesOverwritten", Schema::Int().Min(0))
                                .Prop("lateCompletions", Schema::Int().Min(0))
                                .Prop("partialFramesDiscarded", Schema::Int().Min(0))
                                .Prop("retainedBytes", Schema::Int().Min(0))
                                .Prop("capturedFrames", Schema::Int().Min(0))
                                .Prop("path", Schema::String())
                                .Prop("frames", Schema::Array(Schema::Object()
                                                                  .Prop("frameId", Schema::Int())
                                                                  .Prop("frameTimeMs", Schema::Number())
                                                                  .Prop("rendererCpuMs", Schema::NullableNumber())
                                                                  .Prop("rendererFrameId", Schema::Int())
                                                                  .Prop("rendererFrameAdvanced", Schema::Bool())
                                                                  .Prop("gpuMs", Schema::NullableNumber())
                                                                  .Prop("gpuStatus", Schema::String())
                                                                  .Prop("gpuMeasurementFrameId", Schema::Int())
                                                                  .Prop("gpuIssueFrameId", Schema::Int())
                                                                  .Prop("gpuFrameAdvanced", Schema::Bool())
                                                                  .Prop("sameFrameGpuMs", Schema::NullableNumber())
                                                                  .Prop("sameFrameGpuStatus", Schema::String())
                                                                  .Prop("droppedEvents", Schema::Int())
                                                                  .Prop("scopes", Schema::Array(Schema::Object()
                                                                                                    .Prop("name", Schema::String())
                                                                                                    .Prop("nameTruncated", Schema::Bool())
                                                                                                    .Prop("thread", Schema::Int())
                                                                                                    .Prop("depth", Schema::Int())
                                                                                                    .Prop("startMs", Schema::Number())
                                                                                                    .Prop("durationMs", Schema::NullableNumber())
                                                                                                    .Prop("complete", Schema::Bool())))))
                                .Required({ "status" });
        // Snapshot access is marshaled, but encoding and file I/O are not.
        tool.MainMarshaled = false;
        tool.Handler = HandleCpuScopeRing;
        registry.Register(std::move(tool));
    }
} // namespace OloEngine::MCP
