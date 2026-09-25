// =============================================================================
// McpToolsBenchmark.cpp — olo_benchmark_capture (issue #974)
//
// The EDITOR front door for the benchmark capture manifests — most importantly
// the route that runs them under `--rhi=vulkan`, since the test binary's
// headless context is GL-only. Composes existing seams: OpenSceneFromMcp →
// renderer-settings/exposure apply → viewport override → camera pose → warmed
// frames → Benchmark::CaptureAttachment → Benchmark::WriteResultDirectory.
//
// Time is PINNED here the way the test-binary front door pins it (issue
// #1470): Time::SetMockTime(StartTimeSeconds + n * FixedDtSeconds), stepped
// once per frame index across every camera, so the scene clock that drives
// water / foliage / animation reaches the same value at capture as it does
// in the test binary. Before this the host rendered with the live clock and a
// GL-vs-Vulkan comparison compared two different wave phases. The editor may
// render more than one frame per step (its loop does not wait for us), but the
// scene clock integrates clock DELTAS, so the extra frames add 0 and the value
// at capture is exact. What stays best-effort is everything the editor owns
// per frame — its pacing, quality tiering and TAA history length — so
// result.json still records host:"editor-mcp" and the run-twice acceptance
// proof stays on the test-binary front door.
// =============================================================================

#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpSceneControl.h"

#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Utils/PlatformUtils.h"

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
        bool AwaitBenchmarkFrames(IAutomationHost& host, u64 baseFrame, u32 frames)
        {
            const auto deadline = std::chrono::seconds(10) + std::chrono::milliseconds(250) * frames;
            return AwaitRenderedFrames(host, baseFrame, static_cast<int>(frames),
                                       std::chrono::duration_cast<std::chrono::milliseconds>(deadline));
        }

        // Pin the engine clock on the render thread (the thread whose frames
        // read it). Marshalled, not set from this handler's thread, so a frame
        // never reads a half-updated pair of statics.
        void SetMockClock(IAutomationHost& host, f32 seconds)
        {
            // The benchmark budget, not the default one: this is the FIRST marshal
            // a capture makes, and a freshly launched Vulkan editor can still be
            // inside its shader warm-up when it arrives.
            (void)host.MarshalRead([seconds]() -> Json
                                   {
                Time::SetMockTime(seconds);
                return Json{ { "ok", true } }; },
                                   kBenchmarkMarshalTimeout);
        }

        // Releases the pinned clock on EVERY exit path — an early error return
        // or a marshal timeout throwing out of the handler would otherwise
        // leave the whole editor frozen at the capture's time, animations and
        // all. Best effort and non-throwing, like the renderer-state guard.
        struct MockClockReleaseGuard
        {
            IAutomationHost* Host = nullptr;
            ~MockClockReleaseGuard()
            {
                try
                {
                    (void)Host->MarshalRead([]() -> Json
                                            {
                        Time::ClearMockTime();
                        return Json{ { "ok", true } }; },
                                            kBenchmarkMarshalTimeout);
                }
                catch (...)
                {
                }
            }
        };

        ToolResult Handle_BenchmarkCapture(IAutomationHost& host, const Json& args)
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
                return ToolResult::Error("Manifest '" + manifest->Id.ToStdString() + "' does not declare backend '" + backend +
                                         "' in Backends.Supported.");
            }

            if (!host.Context().OpenSceneFromMcp)
            {
                return ToolResult::Error("Scene open is not available in this editor build.");
            }
            if (!host.Context().SetCameraPose)
            {
                return ToolResult::Error("Camera control is not available in this editor build.");
            }

            // ---- Pin the clock BEFORE the scene opens (issue #1470) ----------
            // The scene seeds its animation clock from Time::GetTime() on its
            // first update, so t0 has to be in place first — the test-binary
            // host's order (docs/guides/renderer-benchmarks.md, step 2).
            const f32 clockStart = manifest->StartTimeSeconds;
            const f32 clockDt = manifest->FixedDtSeconds;
            u32 clockFrame = 0;
            SetMockClock(host, clockStart);
            const MockClockReleaseGuard clockGuard{ &host };

            // ---- Open the manifest's scene (same seam as olo_scene_open) ----
            const std::string scenePath = manifest->ScenePath.ToStdString();
            const Json opened = host.MarshalRead(
                [&host, scenePath]() -> Json
                {
                    if (!host.Context().OpenSceneFromMcp)
                    {
                        return Json{ { "__error", "Scene open is not available in this editor build." } };
                    }
                    const McpSceneOpenResult result = host.Context().OpenSceneFromMcp(scenePath);
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
                ShadowSettings PriorShadowSettings;
                f32 PriorRenderScale = 1.0f;
                FString GpuVendor;
                FString GpuRenderer;
            };
            auto applied = std::make_shared<AppliedState>();
            const auto manifestCopy = std::make_shared<Benchmark::BenchmarkManifest>(*manifest);

            // Renderer settings are restored on EVERY exit path, not just the
            // happy one. A marshal timeout THROWS out of this handler (see the
            // comment above), and the capture now clears ShowGrid /
            // ShowLightGizmos / ShowWorldAxisHelper / ShowCameraFrustums in
            // RendererSettings — which EditorLayer::SyncPrefsFromMembers copies
            // into m_Prefs and serialises. So an abandoned capture would leave
            // the user's editor permanently without a grid AND write that into
            // their preferences file. The scene-only disable this replaced was
            // self-healing by accident; this is self-healing on purpose.
            struct RendererStateRestoreGuard
            {
                IAutomationHost* Host = nullptr;
                std::shared_ptr<AppliedState> State;
                bool Armed = false;

                void Disarm() noexcept
                {
                    Armed = false;
                }

                ~RendererStateRestoreGuard()
                {
                    if (!Armed || Host == nullptr)
                    {
                        return;
                    }
                    // Best effort, and never throw out of a destructor: this
                    // runs while an exception is already in flight.
                    try
                    {
                        auto state = State;
                        Host->MarshalRead(
                            [state]() -> Json
                            {
                                Renderer3D::GetRendererSettings() = state->PriorRendererSettings;
                                Renderer3D::GetPostProcessSettings() = state->PriorPostProcessSettings;
                                Renderer3D::GetShadowMap().SetSettings(state->PriorShadowSettings);
                                Renderer3D::ApplyRendererSettings();
                                Renderer3D::SetRenderScale(state->PriorRenderScale);
                                return Json{ { "ok", true } };
                            });
                    }
                    catch (...)
                    {
                    }
                }
            } restoreGuard{ &host, applied, /*Armed=*/false };
            const Json presetAdmission = host.MarshalRead([manifestCopy, backend]() -> Json
                                                          {
                std::string error;
                const bool admitted = Benchmark::ValidatePresetAdmission(*manifestCopy, backend, error);
                return Json{ { "ok", admitted }, { "error", error } }; });
            if (!presetAdmission.value("ok", false))
                return ToolResult::Error(presetAdmission.value("error", std::string("preset admission failed")));

            host.MarshalRead(
                [&host, applied, manifestCopy, isVulkan]() -> Json
                {
                    // Snapshot what this run overwrites so the epilogue can put
                    // the user's editor session back.
                    applied->PriorRendererSettings = Renderer3D::GetRendererSettings();
                    applied->PriorPostProcessSettings = Renderer3D::GetPostProcessSettings();
                    applied->PriorShadowSettings = Renderer3D::GetShadowMap().GetSettings();
                    applied->PriorRenderScale = Renderer3D::GetRenderScale();

                    // The manifest's renderer-side state — ONE shared
                    // implementation with the test-binary front door.
                    Benchmark::ApplyManifestRendererState(*manifestCopy);
                    if (host.Context().SetViewportSizeOverride)
                    {
                        host.Context().SetViewportSizeOverride(manifestCopy->Width, manifestCopy->Height);
                    }
                    RandomUtils::SetGlobalSeed(manifestCopy->Seed);

                    // A benchmark capture is a picture of the SCENE: turn off the
                    // editor-only viewport helpers the editor render path draws
                    // (infinite grid, world-axis helper, light gizmos, frustums).
                    //
                    // These must be cleared in RENDERER SETTINGS, not on the
                    // Scene. EditorLayer re-pushes ShowGrid / ShowLightGizmos /
                    // ShowWorldAxisHelper from RendererSettings onto the active
                    // scene EVERY FRAME, so a scene-level disable is overwritten
                    // before the warm-up renders a single frame and the capture
                    // comes back with the grid and the world axis drawn into it
                    // — which is what a Vulkan capture of the issue-#1239
                    // reference-head fixture actually showed. Clearing the
                    // settings is also self-restoring: PriorRendererSettings is
                    // snapshotted just above and the epilogue puts it back.
                    auto& rendererSettings = Renderer3D::GetRendererSettings();
                    rendererSettings.ShowGrid = false;
                    rendererSettings.ShowWorldAxisHelper = false;
                    rendererSettings.ShowLightGizmos = false;
                    rendererSettings.ShowCameraFrustums = false;
                    if (host.Context().GetActiveScene)
                    {
                        if (Ref<Scene> activeScene = host.Context().GetActiveScene())
                        {
                            activeScene->SetGridVisible(false);
                            activeScene->SetWorldAxisHelperVisible(false);
                            activeScene->SetLightGizmosVisible(false);
                            activeScene->SetCameraFrustumsVisible(false);
                        }
                    }

                    if (host.Context().GetCameraPose)
                    {
                        applied->PriorPose = host.Context().GetCameraPose();
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

            // The settings are now overwritten, so the guard becomes live.
            restoreGuard.Armed = true;

            // ---- Per camera: pose, warm, capture --------------------------
            // The editor camera seam controls pose + FOV only — the manifest's
            // Near/Far clips CANNOT be applied through it, so a `Derive:
            // linear-depth` attachment (whose metres come from those clips)
            // would silently decode with the wrong planes in this host. It is
            // skipped with the reason recorded instead; the test-binary front
            // door builds its camera from the manifest clips and is the metric
            // product.
            auto cameraSets = std::make_shared<TArray<Benchmark::CameraCaptureSet>>();
            auto passTimings = std::make_shared<TArray<Benchmark::PassTimingRecord>>();
            // Taken in the SAME marshal as the timings, so the validity block
            // describes the pass list it ships with (#1337 criterion 4).
            auto timingValidity = std::make_shared<Benchmark::TimingValidity>();
            auto resolution = std::make_shared<Benchmark::ResolutionRecord>();
            auto configuration = std::make_shared<Benchmark::AppliedConfiguration>();
            auto measurement = std::make_shared<Benchmark::MeasurementRecord>();
            measurement->DeadlineMs = manifest->MeasurementDeadlineMs;
            const FString backendCopy = backend;
            u32 totalWarmFrames = 0;
            u32 trajectoryFrame = 0;
            bool warmupTimedOut = false;

            for (const auto& cameraSpec : manifest->Cameras)
            {
                const f32 fovDegrees = cameraSpec.FovDegrees;
                const u32 warmFrames = cameraSpec.WarmupFrames.value_or(manifest->WarmupFrames);
                totalWarmFrames += warmFrames;

                // Everything one capture step changes — camera pose, pinned
                // clock, entity motion — plus the frame index the await counts
                // from, in ONE marshal. Every marshal waits for the editor's
                // next frame, so separate ones cost a frame each per step; at
                // the Debug editor's few fps on the integrated scene that was
                // most of a 40-minute capture. Returns the frame index, or
                // nullopt when an EntityMotion target is missing.
                const auto applyStep = [&host, &cameraSpec, manifestCopy, fovDegrees, clockStart, clockDt,
                                        &clockFrame](std::optional<u32> poseFrame, u32 motionFrame) -> std::optional<u64>
                {
                    std::optional<Benchmark::ManifestCameraPose> pose;
                    if (poseFrame)
                        pose = Benchmark::CameraPoseAtFrame(cameraSpec, *poseFrame, manifestCopy->FixedDtSeconds);
                    const f32 seconds = clockStart + static_cast<f32>(clockFrame++) * clockDt;
                    const Json result = host.MarshalRead(
                        [&host, manifestCopy, pose, fovDegrees, seconds, motionFrame]() -> Json
                        {
                            if (pose)
                            {
                                host.Context().SetCameraPose(pose->Position, glm::radians(pose->YawDegrees),
                                                             glm::radians(pose->PitchDegrees), fovDegrees);
                            }
                            Time::SetMockTime(seconds);
                            bool ok = true;
                            if (!manifestCopy->EntityMotions.IsEmpty())
                            {
                                std::string error;
                                Ref<Scene> active = host.Context().GetActiveScene();
                                ok = active && Benchmark::ApplyEntityMotion(*active, *manifestCopy, motionFrame, error);
                            }
                            const u64 frameIndex = host.Context().GetFrameIndex ? host.Context().GetFrameIndex() : 0;
                            return Json{ { "ok", ok }, { "frame", frameIndex } };
                        },
                        kBenchmarkMarshalTimeout);
                    if (!result.value("ok", false))
                        return std::nullopt;
                    return result.value("frame", static_cast<u64>(0));
                };

                // One frame at a time for EVERY camera: the pose (issue #1239)
                // and the pinned clock (issue #1470) both advance BETWEEN live
                // frames, on the schedule the test-binary host walks — both go
                // through CameraPoseAtFrame and t0 + n * dt. A still camera
                // used to wait out its whole warm-up in one await, which is
                // exactly where a frozen or live clock would have diverged
                // from the test binary's. Capturing a moving manifest as a
                // still frame would be a silently wrong picture, which is the
                // one outcome this schema exists to prevent.
                for (u32 frame = 0; frame < warmFrames; ++frame)
                {
                    const std::optional<u64> baseFrame = applyStep(frame, trajectoryFrame++);
                    if (!baseFrame)
                        return ToolResult::Error("EntityMotion target missing from the active scene");
                    if (!AwaitBenchmarkFrames(host, *baseFrame, 1u))
                    {
                        warmupTimedOut = true; // recorded, not fatal — capture what we have
                        break;
                    }
                }

                // Measure after warm-up and before attachment readback. Read
                // each completed editor frame separately so the profiler's
                // 300-frame ring cannot silently discard the tail of a run.
                for (u32 frame = 0; frame < manifest->MeasurementFrames; ++frame)
                {
                    const std::optional<u32> poseFrame =
                        cameraSpec.Motion ? std::optional<u32>(warmFrames + frame) : std::nullopt;
                    const std::optional<u64> baseFrame = applyStep(poseFrame, trajectoryFrame++);
                    if (!baseFrame)
                        return ToolResult::Error("EntityMotion target missing from the active scene");
                    if (!AwaitBenchmarkFrames(host, *baseFrame, 1u))
                    {
                        warmupTimedOut = true;
                        break;
                    }
                    const FString measuredCamera = cameraSpec.Id;
                    host.MarshalRead([measurement, measuredCamera, frame]() -> Json
                                     {
                        measurement->Frames.Add(Benchmark::SnapshotEditorMeasuredFrame(measuredCamera.ToView(), frame));
                        return Json{ { "ok", true } }; });
                }

                const FString cameraId = cameraSpec.Id;
                const Benchmark::CaptureContext captureContext{ cameraSpec.NearClip, cameraSpec.FarClip };
                host.MarshalRead(
                    [&host, cameraSets, manifestCopy, cameraId, backendCopy, captureContext]() -> Json
                    {
                        const u32 captureFrame = host.Context().GetFrameIndex
                                                     ? static_cast<u32>(host.Context().GetFrameIndex())
                                                     : 0u;
                        auto set = Benchmark::CaptureCameraSet(*manifestCopy, cameraId.ToView(), captureFrame, backendCopy.ToView(),
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
                        cameraSets->Add(std::move(set));
                        return Json{ { "ok", true } };
                    },
                    kBenchmarkMarshalTimeout);
            }

            auto counters = std::make_shared<Benchmark::RendererCounters>();
            host.MarshalRead(
                [&host, applied, passTimings, timingValidity, resolution, counters, configuration]() -> Json
                {
                    *passTimings = Benchmark::SnapshotPassTimings();
                    *timingValidity = Benchmark::SnapshotTimingValidity();
                    // Read BEFORE the restore below puts the editor's own
                    // render scale and viewport override back: afterwards the
                    // graph would report the editor's dimensions, not the
                    // benchmark's.
                    *resolution = Benchmark::SnapshotResolution();
                    *counters = Benchmark::SnapshotRendererCounters();
                    *configuration = Benchmark::SnapshotAppliedConfiguration();
                    // Put the user's editor session back: camera, renderer +
                    // post-process configuration, render scale, viewport
                    // override, and the viewport helpers (restored to their
                    // editor defaults — the benchmark scene is still open, so
                    // "prior" toggles belong to a scene that is gone).
                    if (applied->PriorPose && host.Context().RestoreCameraPose)
                    {
                        host.Context().RestoreCameraPose(*applied->PriorPose);
                    }
                    Renderer3D::GetPostProcessSettings() = applied->PriorPostProcessSettings;
                    Renderer3D::GetShadowMap().SetSettings(applied->PriorShadowSettings);
                    Renderer3D::GetRendererSettings() = applied->PriorRendererSettings;
                    Renderer3D::ApplyRendererSettings();
                    Renderer3D::SetRenderScale(applied->PriorRenderScale);
                    if (host.Context().SetViewportSizeOverride)
                    {
                        host.Context().SetViewportSizeOverride(0, 0); // clear the override
                    }
                    if (host.Context().GetActiveScene)
                    {
                        // Restore from the SNAPSHOT, not to `true`. Hardcoding
                        // true switched the grid and the gizmos back on for a
                        // user who had deliberately turned them off before
                        // asking for a capture.
                        const auto& prior = applied->PriorRendererSettings;
                        if (Ref<Scene> activeScene = host.Context().GetActiveScene())
                        {
                            activeScene->SetGridVisible(prior.ShowGrid);
                            activeScene->SetWorldAxisHelperVisible(prior.ShowWorldAxisHelper);
                            activeScene->SetLightGizmosVisible(prior.ShowLightGizmos);
                            activeScene->SetCameraFrustumsVisible(prior.ShowCameraFrustums);
                        }
                    }
                    return Json{ { "ok", true } };
                });

            // The epilogue above did the restore; the guard must not repeat it.
            restoreGuard.Disarm();

            // ---- Result directory -----------------------------------------
            Benchmark::RunInfo runInfo;
            runInfo.Backend = backend;
            runInfo.GpuVendor = applied->GpuVendor;
            runInfo.GpuRenderer = applied->GpuRenderer;
            runInfo.CommitSha = Benchmark::QueryCommitShaViaGit();
            runInfo.MachineTag = Benchmark::ResolveMachineTag({});
            runInfo.Host = "editor-mcp";
            runInfo.TotalFramesRendered = totalWarmFrames;
            // Carried into result.json so a reader of the capture sees it too — the
            // MCP summary alone is not part of the result directory, and capturedPose
            // is derived from the DECLARED frame count.
            runInfo.WarmupTimedOut = warmupTimedOut;
            // The last value applyStep() set — the time the final frame rendered at.
            runInfo.FinalMockTimeSeconds =
                clockStart + static_cast<f32>(clockFrame > 0u ? clockFrame - 1u : 0u) * clockDt;
            runInfo.PassTimings = *passTimings;
            runInfo.Timing = *timingValidity;
            runInfo.Resolution = *resolution;
            runInfo.Counters = *counters;
            runInfo.Measurement = *measurement;
            runInfo.Configuration = *configuration;

            // A distinct default from the test-binary front door, so the two
            // hosts' results never overwrite each other.
            const fs::path outDir = args.contains("outDir") && args["outDir"].is_string()
                                        ? fs::path(args["outDir"].get<std::string>())
                                        : fs::path("assets") / "benchmark" / "captures" /
                                              (manifest->Id.ToStdString() + "-editor-" + backend);
            std::string writeError;
            if (!Benchmark::WriteResultDirectory(*manifest, manifestPath, outDir, std::span{ cameraSets->GetData(), static_cast<sizet>(cameraSets->Num()) }, runInfo, writeError))
            {
                return ToolResult::Error("Result directory write failed: " + writeError);
            }

            Json summary;
            summary["id"] = manifest->Id.ToStdString();
            summary["backend"] = backend;
            summary["host"] = "editor-mcp";
            summary["outDir"] = outDir.generic_string();
            summary["cameras"] = Json::array();
            u32 failures = 0;
            for (const auto& set : *cameraSets)
            {
                Json cameraJson;
                cameraJson["id"] = set.CameraId.ToStdString();
                cameraJson["attachments"] = Json::array();
                for (const auto& attachment : set.Attachments)
                {
                    Json a;
                    a["name"] = attachment.Spec.Name.ToStdString();
                    if (attachment.SkippedUnsupported)
                    {
                        a["skipped"] = true;
                    }
                    else if (!attachment.Error.IsEmpty())
                    {
                        a["error"] = attachment.Error.ToStdString();
                        ++failures;
                    }
                    else
                    {
                        a["file"] = attachment.FileName.ToStdString();
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
                "editor-mcp host: the scene clock is pinned to the manifest's StartTimeSeconds + n * FixedDtSeconds "
                "like the test binary's, but frame pacing, quality tiering and TAA history stay the live editor's — "
                "the deterministic run-twice product is the test binary's --olo-capture-manifest front door "
                "(docs/guides/renderer-benchmarks.md)";
            return ToolResult::Structured(summary);
        }
    } // namespace

    void RegisterBenchmarkTools(AutomationRegistry& registry)
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
            "is GL-only and is the DETERMINISTIC one — this host pins the scene clock to the manifest's "
            "Determinism block like the test binary does, but the "
            "editor still owns its frame pacing, viewport-helper toggles and quality tiering per frame, so editor chrome or "
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
                                            "True when a warm-up OR measurement frame wait hit its deadline "
                                            "(10 s + 250 ms per frame). A measurement timeout ends that "
                                            "camera's sampling early, so it has fewer measured frames than "
                                            "the manifest declares (see each scenario's sampleCount) and the "
                                            "run is not a valid budget sample."))
                .Required({ "id", "backend", "host", "outDir", "attachmentFailures", "warmupTimedOut" });
        tool.MainMarshaled = true;
        tool.Handler = Handle_BenchmarkCapture;
        registry.Register(std::move(tool));
    }
} // namespace OloEngine::MCP
