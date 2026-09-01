// =============================================================================
// McpToolsBenchmark.cpp — olo_benchmark_capture (issue #974)
//
// The EDITOR front door for the benchmark capture manifests — most importantly
// the route that runs them under `--rhi=vulkan`, since the test binary's
// headless context is GL-only. Composes existing seams: OpenSceneFromMcp →
// renderer-settings/exposure apply → viewport override → camera pose → warmed
// frames → Benchmark::CaptureAttachment → Benchmark::WriteResultDirectory.
//
// Determinism here is BEST-EFFORT: the live editor renders with real dt and
// its own frame pacing, so the mock-clock stepping the test-binary front door
// performs does not apply. result.json records host:"editor-mcp", so an
// editor-host result can never be mistaken for the deterministic test-binary
// product. The run-twice acceptance proof lives on the test-binary front door;
// this one exists for backend parity and live inspection.
// =============================================================================

#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpSceneControl.h"

#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Scene/Scene.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace OloEngine::MCP
{
    namespace
    {
        namespace fs = std::filesystem;

        // Scene open + a full warm-up can legitimately take minutes on a heavy
        // scene; same rationale as the scene-control timeout.
        constexpr std::chrono::milliseconds kBenchmarkMarshalTimeout{ 120000 };

        // The shared settle helper with a deadline scaled to the declared
        // warm-up frame count (a benchmark waits 128+ frames, not a 2-3 frame
        // screenshot settle).
        bool AwaitBenchmarkFrames(McpServer& server, u64 baseFrame, u32 frames)
        {
            const auto deadline = std::chrono::seconds(10) + std::chrono::milliseconds(250) * frames;
            return AwaitRenderedFrames(server, baseFrame, static_cast<int>(frames),
                                       std::chrono::duration_cast<std::chrono::milliseconds>(deadline));
        }

        u64 CurrentFrame(McpServer& server)
        {
            if (!server.Context().GetFrameIndex)
            {
                return 0;
            }
            return server
                .MarshalRead([&server]() -> Json
                             { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                .value("frame", static_cast<u64>(0));
        }

        ToolResult Handle_BenchmarkCapture(McpServer& server, const Json& args)
        {
            if (!args.contains("manifest") || !args["manifest"].is_string())
            {
                return ToolResult::Error("Missing required argument 'manifest' (a benchmark capture manifest "
                                         ".yaml, e.g. \"assets/benchmark/manifests/material-lab.golden.yaml\").");
            }
            const fs::path manifestPath(args["manifest"].get<std::string>());
            if (!fs::exists(manifestPath))
            {
                return ToolResult::Error("Manifest not found: " + manifestPath.string() +
                                         " (relative paths resolve against the editor working directory, "
                                         "OloEditor/).");
            }

            std::string parseError;
            const auto manifest = Benchmark::LoadBenchmarkManifest(manifestPath, parseError);
            if (!manifest.has_value())
            {
                return ToolResult::Error("Manifest failed validation:\n" + parseError);
            }

            const bool isVulkan = RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
            const std::string backend = isVulkan ? "vulkan" : "opengl";
            if (!manifest->SupportsBackend(backend))
            {
                return ToolResult::Error("Manifest '" + manifest->Id + "' does not declare backend '" + backend +
                                         "' in Backends.Supported.");
            }

            if (!server.Context().OpenSceneFromMcp)
            {
                return ToolResult::Error("Scene open is not available in this editor build.");
            }
            if (!server.Context().SetCameraPose)
            {
                return ToolResult::Error("Camera control is not available in this editor build.");
            }

            // ---- Open the manifest's scene (same seam as olo_scene_open) ----
            const std::string scenePath = manifest->ScenePath;
            const Json opened = server.MarshalRead(
                [&server, scenePath]() -> Json
                {
                    if (!server.Context().OpenSceneFromMcp)
                    {
                        return Json{ { "__error", "Scene open is not available in this editor build." } };
                    }
                    const McpSceneOpenResult result = server.Context().OpenSceneFromMcp(scenePath);
                    return SceneControl::ToJson(result);
                },
                kBenchmarkMarshalTimeout);
            if (opened.is_object() && opened.contains("__error"))
            {
                return ToolResult::Error(opened["__error"].get<std::string>());
            }
            if (!opened.value("ok", false))
            {
                return ToolResult::Error("Scene failed to open: " + opened.value("message", std::string("(no detail)")));
            }

            // ---- Apply the manifest's renderer-side state + provenance reads.
            // A shared_ptr carries non-JSON state out of the marshaled job (and
            // keeps it alive if the caller-side wait times out while the job is
            // still queued — nothing dequeues an abandoned job; a timeout
            // THROWS out of this handler, so no partial result is ever written).
            struct AppliedState
            {
                std::optional<McpCameraPose> PriorPose;
                RendererSettings PriorRendererSettings;
                PostProcessSettings PriorPostProcessSettings;
                f32 PriorRenderScale = 1.0f;
                std::string GpuVendor;
                std::string GpuRenderer;
            };
            auto applied = std::make_shared<AppliedState>();
            const auto manifestCopy = std::make_shared<Benchmark::BenchmarkManifest>(*manifest);
            server.MarshalRead(
                [&server, applied, manifestCopy, isVulkan]() -> Json
                {
                    // Snapshot what this run overwrites so the epilogue can put
                    // the user's editor session back.
                    applied->PriorRendererSettings = Renderer3D::GetRendererSettings();
                    applied->PriorPostProcessSettings = Renderer3D::GetPostProcessSettings();
                    applied->PriorRenderScale = Renderer3D::GetRenderScale();

                    // The manifest's renderer-side state — ONE shared
                    // implementation with the test-binary front door.
                    Benchmark::ApplyManifestRendererState(*manifestCopy);
                    if (server.Context().SetViewportSizeOverride)
                    {
                        server.Context().SetViewportSizeOverride(manifestCopy->Width, manifestCopy->Height);
                    }
                    RandomUtils::SetGlobalSeed(manifestCopy->Seed);

                    // A benchmark capture is a picture of the SCENE: turn off the
                    // editor-only viewport helpers the editor render path draws
                    // (infinite grid, world-axis helper, light gizmos, frustums).
                    if (server.Context().GetActiveScene)
                    {
                        if (Ref<Scene> activeScene = server.Context().GetActiveScene())
                        {
                            activeScene->SetGridVisible(false);
                            activeScene->SetWorldAxisHelperVisible(false);
                            activeScene->SetLightGizmosVisible(false);
                            activeScene->SetCameraFrustumsVisible(false);
                        }
                    }

                    if (server.Context().GetCameraPose)
                    {
                        applied->PriorPose = server.Context().GetCameraPose();
                    }
                    if (!isVulkan)
                    {
                        if (const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR)))
                        {
                            applied->GpuVendor = vendor;
                        }
                        if (const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
                        {
                            applied->GpuRenderer = renderer;
                        }
                    }
                    return Json{ { "ok", true } };
                },
                kBenchmarkMarshalTimeout);

            // ---- Per camera: pose, warm, capture --------------------------
            // The editor camera seam controls pose + FOV only — the manifest's
            // Near/Far clips CANNOT be applied through it, so a `Derive:
            // linear-depth` attachment (whose metres come from those clips)
            // would silently decode with the wrong planes in this host. It is
            // skipped with the reason recorded instead; the test-binary front
            // door builds its camera from the manifest clips and is the metric
            // product.
            auto cameraSets = std::make_shared<std::vector<Benchmark::CameraCaptureSet>>();
            auto passTimings = std::make_shared<std::vector<Benchmark::PassTimingRecord>>();
            const std::string backendCopy = backend;
            u32 totalWarmFrames = 0;
            bool warmupTimedOut = false;

            for (const auto& cameraSpec : manifest->Cameras)
            {
                const glm::vec3 position = cameraSpec.Position;
                const f32 yawRadians = glm::radians(cameraSpec.YawDegrees);
                const f32 pitchRadians = glm::radians(cameraSpec.PitchDegrees);
                const f32 fovDegrees = cameraSpec.FovDegrees;
                server.MarshalRead(
                    [&server, position, yawRadians, pitchRadians, fovDegrees]() -> Json
                    {
                        server.Context().SetCameraPose(position, yawRadians, pitchRadians, fovDegrees);
                        return Json{ { "ok", true } };
                    });

                const u32 warmFrames = cameraSpec.WarmupFrames.value_or(manifest->WarmupFrames);
                totalWarmFrames += warmFrames;
                if (!AwaitBenchmarkFrames(server, CurrentFrame(server), warmFrames))
                {
                    warmupTimedOut = true; // recorded, not fatal — capture what we have
                }

                const std::string cameraId = cameraSpec.Id;
                const Benchmark::CaptureContext captureContext{ cameraSpec.NearClip, cameraSpec.FarClip };
                server.MarshalRead(
                    [&server, cameraSets, manifestCopy, cameraId, backendCopy, captureContext]() -> Json
                    {
                        const u32 captureFrame = server.Context().GetFrameIndex
                                                     ? static_cast<u32>(server.Context().GetFrameIndex())
                                                     : 0u;
                        auto set = Benchmark::CaptureCameraSet(*manifestCopy, cameraId, captureFrame, backendCopy,
                                                               captureContext);
                        // See the loop-header comment: linear-depth derives are
                        // not metric in this host.
                        for (auto& attachment : set.Attachments)
                        {
                            if (attachment.Spec.Derive == Benchmark::AttachmentDerive::LinearDepth &&
                                !attachment.SkippedUnsupported)
                            {
                                Benchmark::CapturedAttachment skipped;
                                skipped.Spec = attachment.Spec;
                                skipped.SkippedUnsupported = true;
                                skipped.SkipReason =
                                    "editor host cannot pin the camera near/far clips (pose seam has no clip "
                                    "control), so metric linear depth is only available from the test-binary "
                                    "front door";
                                attachment = std::move(skipped);
                            }
                        }
                        cameraSets->push_back(std::move(set));
                        return Json{ { "ok", true } };
                    },
                    kBenchmarkMarshalTimeout);
            }

            auto counters = std::make_shared<Benchmark::RendererCounters>();
            server.MarshalRead(
                [&server, applied, passTimings, counters]() -> Json
                {
                    *passTimings = Benchmark::SnapshotPassTimings();
                    *counters = Benchmark::SnapshotRendererCounters();
                    // Put the user's editor session back: camera, renderer +
                    // post-process configuration, render scale, viewport
                    // override, and the viewport helpers (restored to their
                    // editor defaults — the benchmark scene is still open, so
                    // "prior" toggles belong to a scene that is gone).
                    if (applied->PriorPose && server.Context().RestoreCameraPose)
                    {
                        server.Context().RestoreCameraPose(*applied->PriorPose);
                    }
                    Renderer3D::GetPostProcessSettings() = applied->PriorPostProcessSettings;
                    Renderer3D::GetRendererSettings() = applied->PriorRendererSettings;
                    Renderer3D::ApplyRendererSettings();
                    Renderer3D::SetRenderScale(applied->PriorRenderScale);
                    if (server.Context().SetViewportSizeOverride)
                    {
                        server.Context().SetViewportSizeOverride(0, 0); // clear the override
                    }
                    if (server.Context().GetActiveScene)
                    {
                        if (Ref<Scene> activeScene = server.Context().GetActiveScene())
                        {
                            activeScene->SetGridVisible(true);
                            activeScene->SetWorldAxisHelperVisible(true);
                            activeScene->SetLightGizmosVisible(true);
                            activeScene->SetCameraFrustumsVisible(true);
                        }
                    }
                    return Json{ { "ok", true } };
                });

            // ---- Result directory -----------------------------------------
            Benchmark::RunInfo runInfo;
            runInfo.Backend = backend;
            runInfo.GpuVendor = applied->GpuVendor;
            runInfo.GpuRenderer = applied->GpuRenderer;
            runInfo.CommitSha = Benchmark::QueryCommitShaViaGit();
            runInfo.MachineTag = Benchmark::ResolveMachineTag({});
            runInfo.Host = "editor-mcp";
            runInfo.TotalFramesRendered = totalWarmFrames;
            runInfo.FinalMockTimeSeconds = 0.0f; // live clock — no mock stepping in this host
            runInfo.PassTimings = *passTimings;
            runInfo.Counters = *counters;

            // A distinct default from the test-binary front door, so the two
            // hosts' results never overwrite each other.
            const fs::path outDir = args.contains("outDir") && args["outDir"].is_string()
                                        ? fs::path(args["outDir"].get<std::string>())
                                        : fs::path("assets") / "benchmark" / "captures" /
                                              (manifest->Id + "-editor-" + backend);
            std::string writeError;
            if (!Benchmark::WriteResultDirectory(*manifest, manifestPath, outDir, *cameraSets, runInfo, writeError))
            {
                return ToolResult::Error("Result directory write failed: " + writeError);
            }

            Json summary;
            summary["id"] = manifest->Id;
            summary["backend"] = backend;
            summary["host"] = "editor-mcp";
            summary["outDir"] = outDir.generic_string();
            summary["cameras"] = Json::array();
            u32 failures = 0;
            for (const auto& set : *cameraSets)
            {
                Json cameraJson;
                cameraJson["id"] = set.CameraId;
                cameraJson["attachments"] = Json::array();
                for (const auto& attachment : set.Attachments)
                {
                    Json a;
                    a["name"] = attachment.Spec.Name;
                    if (attachment.SkippedUnsupported)
                    {
                        a["skipped"] = true;
                    }
                    else if (!attachment.Error.empty())
                    {
                        a["error"] = attachment.Error;
                        ++failures;
                    }
                    else
                    {
                        a["file"] = attachment.FileName;
                        a["width"] = attachment.Width;
                        a["height"] = attachment.Height;
                    }
                    cameraJson["attachments"].push_back(std::move(a));
                }
                summary["cameras"].push_back(std::move(cameraJson));
            }
            summary["attachmentFailures"] = failures;
            summary["warmupTimedOut"] = warmupTimedOut;
            summary["determinismNote"] =
                "editor-mcp host: live clock, no mock-time stepping — the deterministic run-twice product is the "
                "test binary's --olo-capture-manifest front door (docs/guides/renderer-benchmarks.md)";
            return ToolResult::Structured(summary);
        }
    } // namespace

    void RegisterBenchmarkTools(McpServer& server)
    {
        ToolDef tool;
        tool.Name = "olo_benchmark_capture";
        tool.Toolset = "render";
        tool.Title = "Run a benchmark capture manifest";
        // A project-WRITE tool: it switches the active scene and reconfigures
        // renderer settings/viewport, so it is gated behind "Allow writes".
        // Not idempotent (each run re-opens the scene and rewrites the result
        // directory); not destructive to project files (results are
        // git-ignored; the source scene is untouched).
        tool.ProjectWrite = true;
        tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
        tool.Description =
            "Run an issue-#974 benchmark capture manifest inside the LIVE editor: open its scene, apply its "
            "renderer settings/exposure/viewport, pose each declared camera, warm the declared number of frames, "
            "capture every declared attachment (beauty + AOVs, PNG and Radiance .hdr at native resolution) and "
            "write the self-describing result directory (files + manifest echo + result.json with pass timings "
            "and provenance). Pass 'manifest' (path to a .yaml under assets/benchmark/manifests/, resolved "
            "against OloEditor/) and optionally 'outDir'. This editor front door exists mainly to run manifests "
            "under --rhi=vulkan (the headless test-binary front door, OloEngine-Tests --olo-capture-manifest=, "
            "is GL-only and is the DETERMINISTIC one — this host renders with the live clock, best-effort; the "
            "editor also owns its viewport-helper toggles and quality tiering per frame, so editor chrome or "
            "tiering-adjusted post settings can appear in this host's frames, and the viewport-size override may "
            "not take on every backend — per-attachment dims in result.json record what was actually captured). "
            "This is a WRITE tool: refused unless 'Allow writes' is enabled in the editor's MCP Server panel.";
        tool.InputSchema =
            Schema::Object()
                .Prop("manifest", Schema::String().Desc(
                                      "Path to the capture manifest .yaml (relative paths resolve against "
                                      "OloEditor/, e.g. assets/benchmark/manifests/material-lab.golden.yaml)."))
                .Prop("outDir", Schema::String().Desc(
                                    "Override the result directory (default: assets/benchmark/captures/"
                                    "<Id>-editor-<backend>/)."))
                .Required({ "manifest" });
        tool.OutputSchema =
            Schema::Object()
                .Prop("id", Schema::String().Desc("Manifest Id."))
                .Prop("backend", Schema::String().Desc("Backend the capture ran under (opengl | vulkan)."))
                .Prop("host", Schema::String().Desc("Always 'editor-mcp' for this front door."))
                .Prop("outDir", Schema::String().Desc("Result directory written."))
                .Prop("attachmentFailures", Schema::Int().Min(0).Desc("Attachments that failed to capture."))
                .Prop("warmupTimedOut", Schema::Bool().Desc(
                                            "True when a warm-up wait hit its deadline before the declared "
                                            "frame count (capture proceeded on whatever had rendered)."))
                .Required({ "id", "backend", "host", "outDir", "attachmentFailures", "warmupTimedOut" });
        tool.MainMarshaled = true;
        tool.Handler = Handle_BenchmarkCapture;
        server.RegisterTool(std::move(tool));
    }
} // namespace OloEngine::MCP
