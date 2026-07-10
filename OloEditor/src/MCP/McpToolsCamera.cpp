#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include <algorithm>
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

            int settleFrames = 2;
            if (args.contains("settleFrames") && args["settleFrames"].is_number_integer())
                settleFrames = static_cast<int>(std::clamp<long long>(args["settleFrames"].get<long long>(), 1, 30));

            bool posed = false;
            Json savedPose; // round-trips the prior pose through JSON for the restore job
            bool waitTimedOut = false;
            if (hasCamera || hasOrbit)
            {
                // 1. Save the user's pose and apply the requested one.
                const Json applied = server.MarshalRead([&server, request]() -> Json
                                                        {
                    const McpCameraPose prior = server.Context().GetCameraPose();
                    ApplyCameraRequest(server.Context(), request);
                    Json j;
                    j["focalPoint"] = Json::array({ prior.FocalPoint.x, prior.FocalPoint.y, prior.FocalPoint.z });
                    j["distance"] = prior.Distance;
                    j["yaw"] = prior.YawRadians;
                    j["pitch"] = prior.PitchRadians;
                    j["fov"] = prior.FovDegrees;
                    j["frame"] = server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0;
                    return j; });
                savedPose = applied;
                posed = true;
            }

            // The restore half of the save/restore contract, runnable from both
            // the success path (inside the capture job) and the error path.
            const auto restorePriorPose = [&server, &savedPose]()
            {
                McpCameraPose prior;
                prior.FocalPoint = glm::vec3{ savedPose["focalPoint"][0].get<f32>(),
                                              savedPose["focalPoint"][1].get<f32>(),
                                              savedPose["focalPoint"][2].get<f32>() };
                prior.Distance = savedPose.value("distance", 0.0f);
                prior.YawRadians = savedPose.value("yaw", 0.0f);
                prior.PitchRadians = savedPose.value("pitch", 0.0f);
                prior.FovDegrees = savedPose.value("fov", 45.0f);
                server.Context().RestoreCameraPose(prior);
            };

            Json marshaled;
            try
            {
                // 2. Wait until the new pose has actually been rendered.
                if (posed)
                    waitTimedOut = !AwaitRenderedFrames(server, savedPose.value("frame", static_cast<u64>(0)), settleFrames);

                // 3. Capture — and restore the user's camera in the same main-thread
                // job, so the restore happens exactly once whatever the capture did.
                marshaled = server.MarshalRead([&server, maxWidth, posed, &restorePriorPose]() -> Json
                                               {
                    const std::vector<u8> png = server.Context().CaptureViewportPng(maxWidth);
                    if (posed)
                        restorePriorPose();
                    if (png.empty())
                        return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                    return Json{ { "b64", Base64Encode(png) }, { "bytes", static_cast<u64>(png.size()) } }; });
            }
            catch (...)
            {
                // Don't leave the user's camera stranded at the capture pose if a
                // marshal failed mid-flow. Best effort — if the editor is truly
                // stalled this will fail too, and the original exception matters more.
                if (posed)
                {
                    try
                    {
                        (void)server.MarshalRead([&restorePriorPose]() -> Json
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
                "captured without disturbing the viewport.";
            tool.InputSchema = Schema::Object()
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                                   .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                                   .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
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
