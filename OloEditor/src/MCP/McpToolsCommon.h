#pragma once

// Internal shared surface of the split McpTools*.cpp translation units
// (issue #357): the cross-domain helpers that used to live in McpTools.cpp's
// anonymous namespace (UUID/vec3 parsing, base64, the camera-request machinery
// shared by the camera / golden-compare / scene-control tools, the tool
// annotation builders) plus the per-domain registration entry points that
// RegisterBuiltinTools (MCP/McpTools.cpp) composes. Editor-internal; not part
// of the public MCP surface (that is McpTools.h).

#include "MCP/McpServer.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace OloEngine::MCP
{
    // Parse a UUID argument that may arrive as a string (preferred — u64 exceeds
    // JSON's safe integer range) or a number. Returns false on a bad value.
    inline bool ParseUuid(const Json& value, u64& out)
    {
        if (value.is_string())
        {
            // Full-consumption parse: reject trailing garbage ("123junk") and a
            // leading sign. std::stoull alone accepts both — it stops at the first
            // non-digit (silently truncating "123junk" to 123) and wraps a "-5"
            // into a huge u64 — so a malformed id would resolve to a real entity.
            const std::string& text = value.get<std::string>();
            if (text.empty() || (text[0] != '0' && !(text[0] >= '1' && text[0] <= '9')))
                return false; // leading '+'/'-'/whitespace or empty
            try
            {
                std::size_t consumed = 0;
                const u64 parsed = std::stoull(text, &consumed);
                if (consumed != text.size())
                    return false; // trailing non-digit characters
                out = parsed;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        if (value.is_number_unsigned())
        {
            out = value.get<u64>();
            return true;
        }
        if (value.is_number_integer())
        {
            // A negative JSON number is never a valid UUID; casting it to u64 would
            // silently produce a huge id. Reject instead.
            const long long signedValue = value.get<long long>();
            if (signedValue < 0)
                return false;
            out = static_cast<u64>(signedValue);
            return true;
        }
        return false;
    }

    inline std::string UuidToString(UUID id)
    {
        return std::to_string(static_cast<u64>(id));
    }

    inline f64 Round2(f64 v)
    {
        return std::round(v * 100.0) / 100.0;
    }

    // (Base64Encode moved to McpServer.h — resources/read's binary `blob`
    // contents variant needs it in the dispatch core, issue #673 Tier 1. Call
    // sites here are unchanged: this header includes McpServer.h.)

    // Monotonic per-process sequence for olo://capture/... resource URIs
    // (issue #673 Tier 1, resource-link delivery). Uniqueness is what matters —
    // RegisterEphemeralResource REPLACES a duplicate URI — ordering is cosmetic.
    inline u64 NextCaptureSequence()
    {
        static std::atomic<u64> s_Next{ 1 };
        return s_Next.fetch_add(1, std::memory_order_relaxed);
    }

    // Publish a captured PNG as an ephemeral olo://capture/<n>/<stem>.png resource
    // and return the resource_link content block that hands it back (issue #673
    // Tier 1 resource-link delivery). Shared by olo_screenshot (McpToolsCamera),
    // olo_render_capture_target and olo_render_compare_golden (McpToolsRender) —
    // the size/sequence/URI generation, ResourceDef + BlobReader construction,
    // ephemeral registration, and link-block assembly were byte-for-byte identical
    // across all three. `stem` names both the URI leaf and the resource name
    // (uri = olo://capture/<n>/<stem>.png, name = <stem>-<n>.png). The chosen URI
    // is stamped into `meta` under "resourceUri" BEFORE returning, so a caller that
    // mirrors meta into a text block reports the same URI. Takes `bytes` by value
    // and moves into the reader closure — pass an owned buffer with std::move.
    inline Json PublishCaptureResourceLink(McpServer& server, std::vector<u8> bytes, const std::string& stem,
                                           const std::string& resourceDescription,
                                           const std::string& linkDescription, Json& meta)
    {
        const u64 sizeBytes = static_cast<u64>(bytes.size());
        const u64 sequence = NextCaptureSequence();
        const std::string uri = "olo://capture/" + std::to_string(sequence) + "/" + stem + ".png";
        const std::string name = stem + "-" + std::to_string(sequence) + ".png";

        ResourceDef capture;
        capture.Uri = uri;
        capture.Name = name;
        capture.Description = resourceDescription;
        capture.MimeType = "image/png";
        capture.SizeBytes = sizeBytes;
        capture.BlobReader = [bytes = std::move(bytes)](McpServer&)
        { return bytes; };
        server.RegisterEphemeralResource(std::move(capture));

        meta["resourceUri"] = uri;
        return ToolResult::ResourceLinkBlock(uri, name, linkDescription, "image/png", sizeBytes);
    }

    // ---- Camera tool helpers (Tier 0, #316) --------------------------------

    // Parse a [x, y, z] JSON array of finite numbers.
    inline bool ParseVec3(const Json& value, glm::vec3& out)
    {
        if (!value.is_array() || value.size() != 3)
            return false;
        for (int i = 0; i < 3; ++i)
        {
            if (!value[static_cast<sizet>(i)].is_number())
                return false;
            const f32 v = value[static_cast<sizet>(i)].get<f32>();
            if (!std::isfinite(v))
                return false;
            out[i] = v;
        }
        return true;
    }

    // Invert EditorCamera's orientation convention. Its forward vector is
    // (cos(pitch)·sin(yaw), -sin(pitch), -cos(pitch)·cos(yaw)), so a look
    // direction maps back to yaw = atan2(x, -z), pitch = asin(-y).
    inline void DirectionToYawPitch(const glm::vec3& direction, f32& yawRadians, f32& pitchRadians)
    {
        const glm::vec3 d = glm::normalize(direction);
        yawRadians = std::atan2(d.x, -d.z);
        pitchRadians = std::asin(std::clamp(-d.y, -1.0f, 1.0f));
    }

    inline Json PoseToJson(const McpCameraPose& pose)
    {
        Json j;
        j["position"] = Json::array({ pose.Position.x, pose.Position.y, pose.Position.z });
        j["focalPoint"] = Json::array({ pose.FocalPoint.x, pose.FocalPoint.y, pose.FocalPoint.z });
        j["forward"] = Json::array({ pose.Forward.x, pose.Forward.y, pose.Forward.z });
        j["yawDegrees"] = glm::degrees(pose.YawRadians);
        j["pitchDegrees"] = glm::degrees(pose.PitchRadians);
        j["distance"] = pose.Distance;
        j["fovDegrees"] = pose.FovDegrees;
        j["nearClip"] = pose.NearClip;
        j["farClip"] = pose.FarClip;
        j["viewportWidth"] = pose.ViewportWidth;
        j["viewportHeight"] = pose.ViewportHeight;
        return j;
    }

    // A camera placement request shared by olo_camera_set_pose, olo_camera_orbit
    // and olo_screenshot's optional camera argument.
    struct CameraRequest
    {
        bool IsOrbit = false;
        glm::vec3 EyeOrTarget{ 0.0f }; // eye (pose) or orbit target
        f32 YawRadians = 0.0f;
        f32 PitchRadians = 0.0f;
        f32 Distance = 10.0f;   // orbit only
        f32 FovDegrees = -1.0f; // <= 0 keeps the current FOV
    };

    // Parse {position, target|yaw+pitch, fov} (pose form). Returns an error
    // message, or empty on success.
    inline std::string ParsePoseRequest(const Json& args, CameraRequest& out)
    {
        out.IsOrbit = false;
        if (!args.contains("position") || !ParseVec3(args["position"], out.EyeOrTarget))
            return "Missing or invalid 'position': expected [x, y, z] finite numbers.";

        const bool hasTarget = args.contains("target");
        const bool hasAngles = args.contains("yaw") || args.contains("pitch");
        if (hasTarget && hasAngles)
            return "Give either 'target' or 'yaw'/'pitch', not both.";
        if (hasTarget)
        {
            glm::vec3 target{ 0.0f };
            if (!ParseVec3(args["target"], target))
                return "Invalid 'target': expected [x, y, z] finite numbers.";
            const glm::vec3 direction = target - out.EyeOrTarget;
            if (glm::dot(direction, direction) < 1e-12f)
                return "'target' must differ from 'position'.";
            DirectionToYawPitch(direction, out.YawRadians, out.PitchRadians);
        }
        else if (hasAngles)
        {
            if ((args.contains("yaw") && !args["yaw"].is_number()) ||
                (args.contains("pitch") && !args["pitch"].is_number()))
                return "Invalid 'yaw'/'pitch': expected numbers (degrees).";
            const f32 yawDeg = args.value("yaw", 0.0f);
            const f32 pitchDeg = args.value("pitch", 0.0f);
            if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg))
                return "Invalid 'yaw'/'pitch': expected finite numbers (degrees).";
            out.YawRadians = glm::radians(yawDeg);
            out.PitchRadians = glm::radians(pitchDeg);
        }
        else
        {
            return "Missing look direction: give 'target' [x,y,z] or 'yaw'/'pitch' (degrees).";
        }

        if (args.contains("fov"))
        {
            if (!args["fov"].is_number())
                return "Invalid 'fov': expected a number (degrees).";
            const f32 fov = args.value("fov", 0.0f);
            if (!std::isfinite(fov) || fov < 1.0f || fov > 170.0f)
                return "Invalid 'fov': expected degrees in [1, 170].";
            out.FovDegrees = fov;
        }
        return {};
    }

    // Parse {target, yaw, pitch, distance} (orbit form).
    inline std::string ParseOrbitRequest(const Json& args, CameraRequest& out)
    {
        out.IsOrbit = true;
        if (!args.contains("target") || !ParseVec3(args["target"], out.EyeOrTarget))
            return "Missing or invalid 'target': expected [x, y, z] finite numbers.";
        if ((args.contains("yaw") && !args["yaw"].is_number()) ||
            (args.contains("pitch") && !args["pitch"].is_number()) ||
            (args.contains("distance") && !args["distance"].is_number()))
            return "Invalid 'yaw'/'pitch'/'distance': expected numbers.";
        const f32 yawDeg = args.value("yaw", 0.0f);
        const f32 pitchDeg = args.value("pitch", 30.0f);
        const f32 distance = args.value("distance", 10.0f);
        if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg) || !std::isfinite(distance) || distance <= 0.0f)
            return "Invalid 'yaw'/'pitch'/'distance': expected finite numbers, distance > 0.";
        out.YawRadians = glm::radians(yawDeg);
        out.PitchRadians = glm::radians(pitchDeg);
        out.Distance = distance;
        if (args.contains("fov"))
        {
            if (!args["fov"].is_number())
                return "Invalid 'fov': expected a number (degrees).";
            const f32 fov = args.value("fov", 0.0f);
            if (!std::isfinite(fov) || fov < 1.0f || fov > 170.0f)
                return "Invalid 'fov': expected degrees in [1, 170].";
            out.FovDegrees = fov;
        }
        return {};
    }

    inline bool CameraContextAvailable(const EditorMcpContext& ctx)
    {
        return ctx.GetCameraPose && ctx.SetCameraPose && ctx.OrbitCamera && ctx.RestoreCameraPose;
    }

    // Apply a CameraRequest. MUST run on the main thread (inside MarshalRead).
    inline void ApplyCameraRequest(const EditorMcpContext& ctx, const CameraRequest& request)
    {
        if (request.IsOrbit)
        {
            if (request.FovDegrees > 0.0f)
                ctx.SetCameraPose(glm::vec3(0.0f), 0.0f, 0.0f, request.FovDegrees); // FOV only; pose follows
            ctx.OrbitCamera(request.EyeOrTarget, request.YawRadians, request.PitchRadians, request.Distance);
        }
        else
        {
            ctx.SetCameraPose(request.EyeOrTarget, request.YawRadians, request.PitchRadians, request.FovDegrees);
        }
    }

    // Wait until the editor has rendered `settleFrames` frames after
    // `baseFrame` and the framebuffer is capture-ready (not throttle-skipped
    // and not mid viewport-resize transient), so a camera change is actually
    // visible in the framebuffer before a capture. Returns false on timeout —
    // and also on MCP cancellation (#357 item B): callers that must distinguish
    // check server.IsCurrentCallCancelled() and abort instead of capturing.
    // Emits notifications/progress as frames advance when the caller opted in
    // via a progressToken (a no-op otherwise).
    inline bool AwaitRenderedFrames(McpServer& server, u64 baseFrame, int settleFrames)
    {
        if (!server.Context().GetFrameIndex || !server.Context().IsCaptureUnready)
            return true; // older context: best effort, capture immediately
        const u64 targetFrame = baseFrame + static_cast<u64>(settleFrames);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        u64 lastReported = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (server.IsCurrentCallCancelled())
                return false;
            const Json state = server.MarshalRead([&server]() -> Json
                                                  { return Json{ { "frame", server.Context().GetFrameIndex() },
                                                                 { "unready", server.Context().IsCaptureUnready() } }; });
            const u64 frame = state.value("frame", static_cast<u64>(0));
            if (const u64 rendered = frame > baseFrame ? frame - baseFrame : 0;
                rendered > lastReported && rendered <= static_cast<u64>(settleFrames))
            {
                lastReported = rendered;
                server.EmitProgress(static_cast<f64>(rendered), static_cast<f64>(settleFrames),
                                    "settling frames before capture (" + std::to_string(rendered) + "/" +
                                        std::to_string(settleFrames) + ")");
            }
            if (frame >= targetFrame && !state.value("unready", false))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // ---- MCP ToolAnnotations builders (spec 2025-06-18) ---------------------
    // Behavioural hints emitted under a tool's `annotations`. Every tool here
    // sets openWorldHint:false — they all act on the local editor session, not
    // an "open world" of external systems (the spec's web-search example).

    // A tool that does not modify its environment — the signal a client uses to
    // auto-approve a diagnostic read. The frame/target capture tools count as
    // read-only: they trigger a transient one-frame diagnostic readback that
    // changes no camera, setting, file, or scene state the caller can observe.
    inline Json ReadOnlyAnnotations()
    {
        return Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
    }

    // A tool that mutates transient editor/session state (camera pose, viewport
    // size, ephemeral render overrides, a shader recompile) — additive only,
    // never overwriting data, so destructiveHint:false is emitted.
    //   idempotent — the same arguments leave the editor in the same state
    //                (the camera / viewport setters); omitted otherwise (a
    //                toggle that flips, a reload that re-reads from disk).
    //                Meaningful only while readOnlyHint is false, which holds.
    inline Json MutatingAnnotations(bool idempotent)
    {
        Json a{ { "readOnlyHint", false }, { "openWorldHint", false }, { "destructiveHint", false } };
        if (idempotent)
            a["idempotentHint"] = true;
        return a;
    }

    // A mutating tool that may also overwrite or destroy data (e.g. rebasing a
    // golden PNG): destructiveHint is omitted, keeping the spec default of true.
    inline Json DestructiveMutatingAnnotations()
    {
        return Json{ { "readOnlyHint", false }, { "openWorldHint", false } };
    }

    // ---- per-domain registration (one function per McpTools*.cpp TU) -------
    // RegisterBuiltinTools calls these in a fixed order; each appends its
    // domain's tools/resources in a stable within-domain order, so tools/list
    // is grouped by toolset.

    void RegisterDiagnosticsTools(McpServer& server);
    void RegisterSceneTools(McpServer& server);
    void RegisterPerfTools(McpServer& server);
    void RegisterRenderTools(McpServer& server);
    void RegisterShaderTools(McpServer& server);
    void RegisterAssetTools(McpServer& server);
    void RegisterScriptingTools(McpServer& server);
    void RegisterCameraTools(McpServer& server);
    void RegisterPhysicsTools(McpServer& server);
    void RegisterInputTools(McpServer& server);
} // namespace OloEngine::MCP
