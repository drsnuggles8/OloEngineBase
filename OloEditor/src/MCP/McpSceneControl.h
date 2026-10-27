#pragma once

// Shared, editor-side schema + result shaping for the consented MCP scene-control
// write tools (issue #316): `olo_scene_open` (open/switch the active scene),
// `olo_scene_play`, `olo_scene_simulate`, and `olo_scene_stop` (select the editor's
// runtime mode). Together they give an
// agent scriptable control over which scene is loaded and whether the runtime is
// simulating — the "scriptable repro setup" the read-only server couldn't drive
// (previously a manual Sandbox.oloproj StartScene edit + editor relaunch per scene,
// and an OLO_EDITOR_AUTOPLAY=1 relaunch to reach Play).
//
// Like McpReloadScript.h, the ACTIONS cannot live in this header — opening a scene
// and toggling Play touch the EnTT registry / renderer / runtime, which a unit test
// must not invoke. So they are routed through the EditorMcpContext hooks
// (OpenSceneFromMcp / SetScenePlayState): the editor wires them to its Open Scene /
// OnScenePlay / OnSceneSimulate / OnSceneStop paths, a headless host leaves them null ("not
// available"), and a test injects a fake. What stays here, header-only and
// engine-free, is the bit the tests pin: the tools' inputSchema, the pure
// scene-path validation (extension + traversal guard), and the result -> JSON
// shaping the handlers emit.
//
// Engine-free: only McpServer.h (the McpSceneOpenResult / McpScenePlayResult structs
// + Json) and the schema-builder DSL, so it pulls no extra editor TU into the MCP
// test binary (which deliberately does NOT link McpTools.cpp) — the same split
// McpReloadScript.h / McpRendererSettings.h use.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::MCP::SceneControl
{
    using Json = nlohmann::json;

    // The lowercase extension of `path` including the leading dot ("" if none). Pure
    // string work — no filesystem access — so it is unit-testable and pulls in no
    // std::filesystem. Only the final path component's last dot counts.
    [[nodiscard]] inline std::string LowercaseExtension(std::string_view path)
    {
        // Find the start of the final component (after the last '/' or '\\').
        sizet slash = path.find_last_of("/\\");
        const std::string_view leaf = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
        const sizet dot = leaf.find_last_of('.');
        if (dot == std::string_view::npos || dot == 0) // no dot, or a dotfile like ".gitignore"
            return {};
        std::string ext(leaf.substr(dot));
        // Scene extensions are ASCII (.olo / .scene), so a locale-independent
        // ASCII fold is correct and avoids <cctype>'s locale dependence.
        for (char& c : ext)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return ext;
    }

    // Validate a scene path argument for `olo_scene_open`. Pure: it enforces only
    // what can be checked without the filesystem — a non-empty string with a
    // recognised scene extension (.olo / .scene) and no parent-directory traversal
    // (a ".." component would let a consented write escape the project tree). The
    // editor hook does the rest (resolve relative to the asset directory, check the
    // file exists, deserialize). Returns a human-readable error, or nullopt on OK.
    // True when `path` has a ".." component. Checked between separators so a file
    // name that merely contains two dots ("a..b.olo") is fine. Shared with
    // olo_asset_open (McpAssetOpen.h).
    [[nodiscard]] inline bool HasParentTraversal(std::string_view path)
    {
        sizet start = 0;
        while (start <= path.size())
        {
            const sizet sep = path.find_first_of("/\\", start);
            const std::string_view comp = path.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
            if (comp == "..")
                return true;
            if (sep == std::string_view::npos)
                break;
            start = sep + 1;
        }
        return false;
    }

    [[nodiscard]] inline std::optional<std::string> ValidateScenePath(std::string_view path)
    {
        if (path.empty())
            return "Missing required argument 'path' (a .olo or .scene scene file, relative to the project asset directory).";

        // Reject a ".." path component (traversal out of the project).
        if (HasParentTraversal(path))
            return "Invalid 'path': parent-directory traversal ('..') is not allowed.";

        if (const std::string ext = LowercaseExtension(path); ext != ".olo" && ext != ".scene")
            return "Invalid 'path': expected a scene file ending in .olo or .scene.";
        return std::nullopt;
    }

    // inputSchema for olo_scene_open: a required `path` string. The value-level
    // extension / traversal constraints can't be expressed in JSON-Schema, so
    // ValidateScenePath enforces them and the server's schema check only guarantees
    // `path` is present and a string.
    [[nodiscard]] inline Json OpenInputSchema()
    {
        return Schema::Object()
            .Prop("path", Schema::String().Desc("The scene file to open, ending in .olo or .scene. Relative paths resolve "
                                                "against the project's asset directory (e.g. \"Scenes/Sandbox.olo\"); an absolute "
                                                "path is also accepted. Parent-directory traversal ('..') is rejected."))
            .Prop("wait", Schema::Bool().Desc("Wait for the opened scene to finish loading (default true): its first frames "
                                              "build what it draws, bound coats a few a frame. false returns once the scene "
                                              "is installed; poll olo_scene_load_status until 'loaded' before capturing."))
            .Required({ "path" })
            .NoAdditional();
    }

    // inputSchema for olo_scene_play / olo_scene_simulate / olo_scene_stop: no arguments (the transition
    // direction is fixed by which tool you call, mirroring the editor's Play / Stop
    // buttons).
    [[nodiscard]] inline Json PlayStopInputSchema()
    {
        return Schema::EmptyObject();
    }

    [[nodiscard]] inline Json SceneStateOutputSchema()
    {
        return Schema::Object()
            .Prop("available", Schema::Bool().Desc("False when this editor build has no scene-mode hook."))
            .Prop("ok", Schema::Bool().Desc("Editor is in the requested mode afterwards."))
            .Prop("playing", Schema::Bool().Desc("True only in Play mode."))
            .Prop("simulating", Schema::Bool().Desc("True only in Simulate mode."))
            .Prop("mode", Schema::String().Enum({ "edit", "play", "simulate" }).Desc("Scene mode after the call."))
            .Prop("changed", Schema::Bool().Desc("True only when this call actually transitioned (a no-op is changed:false)."))
            .Prop("sceneName", Schema::String())
            .Prop("message", Schema::String().Desc("Human-readable outcome detail."))
            .Required({ "available", "ok", "playing", "simulating", "mode", "changed", "sceneName", "message" });
    }

    // Result shaping. Each mirrors the hook's outcome struct into the JSON an agent
    // sees. `available` distinguishes "no editor in this host" from a real result.
    [[nodiscard]] inline Json ToJson(const McpSceneOpenResult& result)
    {
        return Json{
            { "available", result.Available },
            { "ok", result.Ok },
            { "path", result.Path },
            { "sceneName", result.SceneName },
            { "entityCount", result.EntityCount },
            { "renderingPath", result.RenderingPath },
            { "message", result.Message },
        };
    }

    // ---- Waiting out a scene load (issue #1591) -----------------------------
    //
    // OPENING A SCENE IS NOT THE END OF LOADING IT. The open job deserializes
    // and installs the scene; the frames after it build what the scene draws --
    // the acceptance scene's 44 bound coats among them, staged a few a frame. A
    // caller that reads the editor with the 5 s default right after the open
    // asks the main thread during those frames, and a frame that outlasts the
    // read reports "Timed out waiting for the editor main thread" for a load
    // that is still progressing. Every olo_benchmark_capture of the acceptance
    // scene failed that way.
    //
    // So a load is waited for by POLLING: a short read per poll, an unanswered
    // poll counted as "the editor is busy with a long frame" rather than as a
    // failure, until `settleFrames` frames have rendered since the editor
    // installed the scene and the groom pass reports no coat awaiting its first
    // build -- or the whole budget runs out, which is reported, not thrown.
    // Counted from the INSTALL, not from a frame the caller read: a poll the
    // main thread answers before the new scene's first frame sees the last
    // scene's groom stats, and must not call the new one loaded.

    // One poll. `Answered` false: the main thread did not take the read within
    // the poll's own timeout, which during a load means a long frame.
    struct SceneLoadProbe
    {
        bool Answered = false;
        u64 FramesSinceOpen = 0;
        u32 GroomsAwaitingBuild = 0;
    };

    [[nodiscard]] inline bool IsSceneLoaded(const SceneLoadProbe& probe, u32 settleFrames) noexcept
    {
        return probe.Answered && probe.FramesSinceOpen >= settleFrames && probe.GroomsAwaitingBuild == 0u;
    }

    struct SceneLoadOutcome
    {
        bool Loaded = false;
        bool Cancelled = false;
        /// Frames rendered since the scene was installed, at the last answered poll.
        u64 FramesRendered = 0;
        /// Coats still awaiting their first build at the last answered poll.
        u32 GroomsAwaitingBuild = 0;
        u32 Polls = 0;
        u32 UnansweredPolls = 0;
        f64 Seconds = 0.0;
    };

    // `probe()` -> SceneLoadProbe, `cancelled()` -> bool, `now()` ->
    // steady_clock::time_point, `sleep(ms)`: injected so the dispatch tests can
    // drive a load without an editor or a real clock.
    template<typename ProbeFn, typename CancelledFn, typename NowFn, typename SleepFn>
    [[nodiscard]] SceneLoadOutcome AwaitSceneLoad(ProbeFn&& probe, CancelledFn&& cancelled, NowFn&& now, SleepFn&& sleep,
                                                  u32 settleFrames, std::chrono::milliseconds budget,
                                                  std::chrono::milliseconds pollInterval = std::chrono::milliseconds(50))
    {
        SceneLoadOutcome outcome;
        const auto start = now();
        const auto deadline = start + budget;
        for (;;)
        {
            if (cancelled())
            {
                outcome.Cancelled = true;
                break;
            }
            const SceneLoadProbe state = probe();
            ++outcome.Polls;
            if (!state.Answered)
            {
                ++outcome.UnansweredPolls;
            }
            else
            {
                outcome.FramesRendered = state.FramesSinceOpen;
                outcome.GroomsAwaitingBuild = state.GroomsAwaitingBuild;
                if (IsSceneLoaded(state, settleFrames))
                {
                    outcome.Loaded = true;
                    break;
                }
            }
            if (now() >= deadline)
            {
                break;
            }
            sleep(pollInterval);
        }
        outcome.Seconds = std::chrono::duration<f64>(now() - start).count();
        return outcome;
    }

    // Frames of the new scene a load must have rendered before it is called
    // loaded, and how long a tool waits for one. Polled, so a slow load costs
    // only the wait and never a timeout that leaves a queued backlog behind.
    inline constexpr u32 kSceneLoadSettleFrames = 2;
    inline constexpr std::chrono::milliseconds kSceneLoadTimeout{ 300000 };

    [[nodiscard]] inline std::string StillLoadingMessage(const SceneLoadOutcome& outcome)
    {
        return "The scene opened but was still loading after " + std::to_string(static_cast<int>(outcome.Seconds)) +
               " s (" + std::to_string(outcome.GroomsAwaitingBuild) + " bound coat(s) awaiting their first build)";
    }

    [[nodiscard]] inline Json ToJson(const SceneLoadOutcome& outcome)
    {
        return Json{
            { "loaded", outcome.Loaded },
            { "cancelled", outcome.Cancelled },
            { "framesRendered", outcome.FramesRendered },
            { "groomsAwaitingBuild", outcome.GroomsAwaitingBuild },
            { "polls", outcome.Polls },
            { "unansweredPolls", outcome.UnansweredPolls },
            { "seconds", outcome.Seconds },
        };
    }

    [[nodiscard]] inline Schema::Node SceneLoadOutputSchema()
    {
        return Schema::Object()
            .Prop("loaded", Schema::Bool().Desc("Frames have rendered since the scene was installed and no bound coat awaits its first build."))
            .Prop("cancelled", Schema::Bool())
            .Prop("framesRendered", Schema::Int().Min(0).Desc("Frames rendered since the scene was installed, at the last answered poll."))
            .Prop("groomsAwaitingBuild", Schema::Int().Min(0).Desc("Bound coats the groom pass has not built yet; they are not drawn until built."))
            .Prop("polls", Schema::Int().Min(0))
            .Prop("unansweredPolls", Schema::Int().Min(0).Desc("Polls the main thread did not take in time: frames that outlasted the poll."))
            .Prop("seconds", Schema::Number().Min(0))
            .Required({ "loaded", "cancelled", "framesRendered", "groomsAwaitingBuild", "polls", "unansweredPolls", "seconds" });
    }

    [[nodiscard]] inline Json ToJson(const McpScenePlayResult& result)
    {
        return Json{
            { "available", result.Available },
            { "ok", result.Ok },
            { "playing", result.Playing },
            { "simulating", result.Simulating },
            { "mode", result.Mode },
            { "changed", result.Changed },
            { "sceneName", result.SceneName },
            { "message", result.Message },
        };
    }
} // namespace OloEngine::MCP::SceneControl
