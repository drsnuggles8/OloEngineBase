#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Camera / viewport MCP tools (Tier 0, issue #316): olo_screenshot with optional
// one-shot pose, the camera get/set/orbit/frame-entity family, and the viewport
// size override. Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_camera_get (main-marshaled) -----------------------------------
        ToolResult Handle_CameraGet(McpServer& server, const Json& /*args*/)
        {
            if (!server.Context().GetCameraPose)
                return ToolResult::Error("Camera control is not available in this editor build.");
            const Json pose = server.MarshalRead([&server]() -> Json
                                                 { return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_set_pose (main-marshaled; mutates editor camera) -------
        ToolResult Handle_CameraSetPose(McpServer& server, const Json& args)
        {
            if (!CameraContextAvailable(server.Context()))
                return ToolResult::Error("Camera control is not available in this editor build.");
            CameraRequest request;
            if (const std::string error = ParsePoseRequest(args, request); !error.empty())
                return ToolResult::Error(error);

            const Json pose = server.MarshalRead([&server, request]() -> Json
                                                 {
                ApplyCameraRequest(server.Context(), request);
                return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_orbit (main-marshaled; mutates editor camera) ----------
        ToolResult Handle_CameraOrbit(McpServer& server, const Json& args)
        {
            if (!CameraContextAvailable(server.Context()))
                return ToolResult::Error("Camera control is not available in this editor build.");
            CameraRequest request;
            if (const std::string error = ParseOrbitRequest(args, request); !error.empty())
                return ToolResult::Error(error);

            const Json pose = server.MarshalRead([&server, request]() -> Json
                                                 {
                ApplyCameraRequest(server.Context(), request);
                return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_frame_entity (main-marshaled; mutates editor camera) ---
        ToolResult Handle_CameraFrameEntity(McpServer& server, const Json& args)
        {
            if (!server.Context().FrameEntity || !server.Context().GetCameraPose)
                return ToolResult::Error("Camera control is not available in this editor build.");
            if (!args.contains("id"))
                return ToolResult::Error("Missing required argument 'id' (entity UUID).");
            u64 idValue = 0;
            if (!ParseUuid(args["id"], idValue))
                return ToolResult::Error("Invalid 'id': expected a UUID as a string or number.");

            const Json result = server.MarshalRead([&server, idValue]() -> Json
                                                   {
                if (!server.Context().FrameEntity(idValue))
                    return Json{ { "__error", "No entity with that UUID in the active scene." } };
                Json j = PoseToJson(server.Context().GetCameraPose());
                j["framedEntity"] = std::to_string(idValue);
                return j; });
            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_viewport_set_size (main-marshaled; mutates viewport override) -
        ToolResult Handle_ViewportSetSize(McpServer& server, const Json& args)
        {
            if (!server.Context().SetViewportSizeOverride)
                return ToolResult::Error("Viewport control is not available in this editor build.");

            u32 width = 0;
            u32 height = 0;
            const bool reset = args.value("reset", false);
            if (!reset)
            {
                if (!args.contains("width") || !args.contains("height") ||
                    !args["width"].is_number_integer() || !args["height"].is_number_integer())
                    return ToolResult::Error("Give 'width' and 'height' (pixels), or 'reset': true.");
                width = static_cast<u32>(std::clamp<long long>(args["width"].get<long long>(), 64, 8192));
                height = static_cast<u32>(std::clamp<long long>(args["height"].get<long long>(), 64, 8192));
            }

            const Json result = server.MarshalRead([&server, width, height, reset]() -> Json
                                                   {
                server.Context().SetViewportSizeOverride(width, height);
                Json j;
                j["override"] = !reset;
                if (!reset)
                {
                    j["width"] = width;
                    j["height"] = height;
                }
                return j; });
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_screenshot (main-marshaled; GL readback) ----------------------
        // Captures the editor viewport framebuffer as a PNG and returns it as an MCP
        // image content block so the agent can SEE the rendered frame. With the
        // optional 'camera'/'orbit' argument it poses the editor camera first, waits
        // for the pose to be rendered, captures, and restores the prior camera —
        // multi-angle inspection without disturbing the user's viewport (#316).
        ToolResult Handle_Screenshot(McpServer& server, const Json& args)
        {
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            if (!server.Context().CaptureViewportPng)
                return ToolResult::Error("Screenshot capture is not available in this editor build.");

            // Optional camera placement for this capture only.
            const bool hasCamera = args.contains("camera");
            const bool hasOrbit = args.contains("orbit");
            if (hasCamera && hasOrbit)
                return ToolResult::Error("Give either 'camera' or 'orbit', not both.");
            CameraRequest request;
            if (hasCamera || hasOrbit)
            {
                if (!CameraContextAvailable(server.Context()))
                    return ToolResult::Error("Camera control is not available in this editor build.");
                const std::string error = hasCamera ? ParsePoseRequest(args["camera"], request)
                                                    : ParseOrbitRequest(args["orbit"], request);
                if (!error.empty())
                    return ToolResult::Error(error);
            }

            // In Play mode the frame is rendered from the runtime's primary
            // CameraComponent — the editor camera the pose machinery moves is
            // not consulted at all, so a posed capture would silently return
            // the runtime view while claiming the requested pose (issue #607,
            // the grey-frame confusion). Refuse honestly instead. The
            // sceneState is also reported alongside every capture below so an
            // un-posed Play capture is never mistaken for an Edit-mode one.
            bool isPlaying = false;
            if (server.Context().IsPlaying)
            {
                isPlaying = server.MarshalRead([&server]() -> Json
                                               { return Json{ { "playing", server.Context().IsPlaying() } }; })
                                .value("playing", false);
            }
            if (isPlaying && (hasCamera || hasOrbit))
                return ToolResult::Error(
                    "A 'camera'/'orbit' pose cannot be applied in Play mode: the frame is rendered from the "
                    "runtime scene's primary CameraComponent, not the editor camera, so the posed capture would "
                    "silently show the runtime view from the runtime camera instead of the requested pose. "
                    "Capture without a pose to see what is playing, or olo_scene_stop first for editor-camera "
                    "captures.");

            int settleFrames = 2;
            if (args.contains("settleFrames") && args["settleFrames"].is_number_integer())
                settleFrames = static_cast<int>(std::clamp<long long>(args["settleFrames"].get<long long>(), 1, 30));

            bool posed = false;
            bool waitTimedOut = false;

            // Save/restore state shared between the apply job, the capture-and-restore
            // job, and the error-path restore (issue #610). The apply job records the
            // user's prior pose and raises `Applied` LAST; every restore path reads
            // `Applied` and restores `Prior` only when it is set. This closes the
            // pose-strand edge: if the step-1 apply marshal times out (throws) its
            // enqueued job may still run and move the camera later — because the
            // prior pose lives here (not in the timed-out future) and the error-path
            // restore is enqueued AFTER the apply job (game-thread tasks drain FIFO),
            // that late apply is always followed by a matching restore instead of
            // stranding the user at the screenshot pose.
            struct PoseGuardState
            {
                std::atomic<bool> Applied{ false };
                McpCameraPose Prior;
            };
            const auto poseState = (hasCamera || hasOrbit) ? std::make_shared<PoseGuardState>() : nullptr;

            // The restore half of the save/restore contract. Captures poseState BY
            // VALUE (a shared_ptr), so a copy carried into a marshaled job keeps the
            // state alive even if this call unwinds before an orphaned job runs. A
            // no-op until the apply job has actually applied the pose.
            const auto restorePriorPose = [&server, poseState]()
            {
                if (poseState && poseState->Applied.load(std::memory_order_acquire))
                    server.Context().RestoreCameraPose(poseState->Prior);
            };

            Json marshaled;
            try
            {
                u64 appliedFrame = 0;
                if (poseState)
                {
                    // 1. Save the user's pose and apply the requested one — inside the
                    // try so a marshal timeout here is caught and restored below.
                    const Json applied = server.MarshalRead([&server, request, poseState]() -> Json
                                                            {
                        poseState->Prior = server.Context().GetCameraPose();
                        ApplyCameraRequest(server.Context(), request);
                        // Publish LAST: `Applied` release-fences the Prior store so any
                        // restore that reads Applied==true also observes Prior.
                        poseState->Applied.store(true, std::memory_order_release);
                        return Json{ { "frame", server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0 } }; });
                    posed = true;
                    appliedFrame = applied.value("frame", static_cast<u64>(0));
                }

                // 2. Wait until the new pose has actually been rendered.
                //
                // Without a pose there is normally nothing to wait for — but the
                // viewport framebuffer still holds whatever was LAST drawn, so a
                // capture taken immediately after a scene open / setting change can
                // return the previous scene's pixels with no error and no clue
                // (issue #607; the same staleness hazard olo_render_capture_target
                // grew 'forceFrame' for). Opting in renders + settles fresh frames
                // first.
                if (posed)
                    waitTimedOut = !AwaitRenderedFrames(server, appliedFrame, settleFrames);
                else if (args.value("forceFrame", false) && server.Context().GetFrameIndex)
                {
                    const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                             { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                              .value("frame", static_cast<u64>(0));
                    waitTimedOut = !AwaitRenderedFrames(server, baseFrame, settleFrames);
                }

                // 3. Capture — and restore the user's camera in the same main-thread
                // job, so the restore happens exactly once whatever the capture did.
                // The play state is RE-SAMPLED here, in the same job as the capture,
                // so the sceneState meta below describes the frame actually captured
                // even if Play/Stop flipped between the early guard and this job.
                marshaled = server.MarshalRead([&server, maxWidth, restorePriorPose]() -> Json
                                               {
                    const std::vector<u8> png = server.Context().CaptureViewportPng(maxWidth);
                    restorePriorPose();
                    if (png.empty())
                        return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                    return Json{ { "b64", Base64Encode(png) },
                                 { "bytes", static_cast<u64>(png.size()) },
                                 { "playing", server.Context().IsPlaying ? server.Context().IsPlaying() : false } }; });
            }
            catch (...)
            {
                // Don't leave the user's camera stranded at the capture pose if a
                // marshal failed mid-flow — including a step-1 apply that timed out
                // here but whose job runs later. `restorePriorPose` (captured by value)
                // no-ops until that apply raises `Applied`, and this restore is
                // enqueued after it (FIFO), so the late apply is always undone. Best
                // effort — if the editor is truly stalled this throws too, and the
                // original exception matters more.
                if (poseState)
                {
                    try
                    {
                        (void)server.MarshalRead([restorePriorPose]() -> Json
                                                 {
                            restorePriorPose();
                            return Json{}; });
                    }
                    catch (...)
                    {
                    }
                }
                throw;
            }

            if (marshaled.is_object() && marshaled.contains("__error"))
                return ToolResult::Error(marshaled["__error"].get<std::string>());

            ToolResult result;
            result.Content = Json::array();
            if (waitTimedOut)
                result.Content.push_back(Json{ { "type", "text" },
                                               { "text", "Warning: timed out waiting for the new camera pose to render; "
                                                         "the image may show a stale frame." } });
            // Which state (and so whose camera) produced this frame — Play
            // renders from the runtime's primary CameraComponent, Edit /
            // Simulate from the editor camera (issue #607: a Play-mode frame
            // must never be misread as an editor-camera one). Read from the
            // capture job's own sample, not the earlier guard's — Play/Stop
            // could have flipped between the two.
            const bool capturedWhilePlaying = marshaled.value("playing", isPlaying);
            Json meta;
            meta["sceneState"] = capturedWhilePlaying ? "play" : "edit-or-simulate";
            meta["camera"] = capturedWhilePlaying ? "runtime primary CameraComponent" : "editor camera";
            result.Content.push_back(Json{ { "type", "text" }, { "text", meta.dump(2) } });
            result.Content.push_back(Json{ { "type", "image" },
                                           { "data", marshaled["b64"] },
                                           { "mimeType", "image/png" } });
            result.IsError = false;
            return result;
        }

    } // namespace

    void RegisterCameraTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_screenshot";
            tool.Toolset = "camera";
            tool.Title = "Screenshot viewport";
            // Read-only without a pose, but with a 'camera'/'orbit' pose it moves the
            // editor camera (saving/restoring it) — a transient mutation. Flag it
            // readOnlyHint:false to be safe so a client doesn't auto-approve a call
            // that nudges the user's viewport; destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Capture the editor's 3D viewport as a PNG image so you can SEE the rendered frame — "
                "decisive for visual problems ('my material looks wrong', 'I can't see my object'). "
                "Returns an image content block (downscaled to maxWidth, default 1024). Optionally pose "
                "the camera for this capture only via 'camera' (explicit position + target/yaw/pitch) or "
                "'orbit' (target + yaw/pitch/distance): the prior camera is saved, the new pose is "
                "rendered ('settleFrames' frames, default 2, for TAA/temporal effects to settle), the "
                "frame is captured, and the user's camera is restored — so multiple angles can be "
                "captured without disturbing the viewport. In PLAY mode the frame is rendered from the "
                "runtime scene's primary CameraComponent, so 'camera'/'orbit' poses are refused there "
                "(the editor camera does not drive Play rendering); the reply's sceneState/camera meta "
                "says which camera produced the frame.";
            tool.InputSchema = Schema::Object()
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                                   .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                                   .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
                                   .Prop("forceFrame", Schema::Bool().Desc("Without a 'camera'/'orbit' pose, render and settle fresh frames before capturing (default false). Use right after a scene open / setting change so the image cannot be a stale frame."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_Screenshot;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_get";
            tool.Toolset = "camera";
            tool.Title = "Get camera pose";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Get the editor camera's current pose: position, focal point, forward vector, yaw/pitch "
                "(degrees), orbit distance, FOV, near/far clip, and the viewport size in logical pixels. "
                "Read this before moving the camera so you can put it back, or to reason about what the "
                "viewport is looking at.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_set_pose";
            tool.Toolset = "camera";
            tool.Title = "Set camera pose";
            // Moves the editor camera; same args => same pose (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Move the editor camera to an explicit pose (Tier-0 inspection state; never touches the "
                "project). Give 'position' plus either 'target' (a point to look at) or 'yaw'/'pitch' in "
                "degrees; optional 'fov' (vertical, degrees). Returns the resulting pose. The viewport "
                "renders the new pose from the next frame.";
            tool.InputSchema = Schema::Object()
                                   .Prop("position", Schema::Vec3("Camera eye position [x, y, z] (world units)."))
                                   .Prop("target", Schema::Vec3("Point to look at [x, y, z]. Alternative to yaw/pitch."))
                                   .Prop("yaw", Schema::Number().Desc("Yaw in degrees (0 looks along -Z; positive turns right). Alternative to target."))
                                   .Prop("pitch", Schema::Number().Desc("Pitch in degrees (positive looks down). Alternative to target."))
                                   .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                                   .Required({ "position" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraSetPose;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_orbit";
            tool.Toolset = "camera";
            tool.Title = "Orbit camera";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Orbit-frame the editor camera around a world-space target point: the camera pivots at "
                "'distance' from 'target' looking at it, with 'yaw'/'pitch' in degrees (positive pitch "
                "looks down). Keeps a live orbit pivot so subsequent user orbiting feels natural. "
                "Returns the resulting pose.";
            tool.InputSchema = Schema::Object()
                                   .Prop("target", Schema::Vec3("Orbit centre [x, y, z] (world units)."))
                                   .Prop("yaw", Schema::Number().Desc("Orbit yaw in degrees (default 0)."))
                                   .Prop("pitch", Schema::Number().Desc("Orbit pitch in degrees, positive looks down (default 30)."))
                                   .Prop("distance", Schema::Number().ExclusiveMin(0).Desc("Distance from the target in world units (default 10)."))
                                   .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                                   .Required({ "target" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraOrbit;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_frame_entity";
            tool.Toolset = "camera";
            tool.Title = "Frame entity in view";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Point the editor camera at one entity and fit it in view (like pressing 'frame selected' "
                "in a DCC tool). Uses the entity's mesh/model/terrain bounds when available, otherwise its "
                "transform scale. Keeps the current view direction. Get the UUID from "
                "olo_scene_list_entities. Returns the resulting pose.";
            tool.InputSchema = Schema::Object()
                                   .Prop("id", Schema::EntityId())
                                   .Required({ "id" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraFrameEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_viewport_set_size";
            tool.Toolset = "camera";
            tool.Title = "Set viewport size";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Override the editor viewport's logical render size for deterministic capture resolution "
                "(e.g. 1280x720 golden-image comparisons), independent of the panel layout. Pass "
                "'reset': true to return control to the ImGui panel size. The override persists until "
                "reset — reset it when you are done capturing.";
            tool.InputSchema = Schema::Object()
                                   .Prop("width", Schema::Int().Min(64).Max(8192).Desc("Viewport width in logical pixels."))
                                   .Prop("height", Schema::Int().Min(64).Max(8192).Desc("Viewport height in logical pixels."))
                                   .Prop("reset", Schema::Bool().Desc("true = clear the override and use the panel size again."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ViewportSetSize;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
