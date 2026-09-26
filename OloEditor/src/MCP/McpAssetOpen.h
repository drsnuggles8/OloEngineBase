#pragma once

// Pure half of olo_asset_open (issue #607): the schema, argument parsing and the
// result shaping. The action itself lives in EditorLayer::OpenAssetInEditor, the
// SAME function the Content Browser's double-click calls, so the MCP route and the
// UI route cannot drift apart — this header only decides what was asked for and
// how to report what happened.
//
// Why the reply carries a readback rather than an acknowledgement: most asset
// editor panels load DEFERRED (the Sound Graph and Shader Graph panels queue the
// path and deserialize it a couple of frames later, after their ImGui window
// exists), and every one of them logs and returns on a parse error without
// telling its caller. "Opened" is therefore only true once the panel reports the
// requested file as the one it has loaded, and the tool waits for that — see
// McpAssetOpenState.
//
// Engine-free (McpServer.h + the schema DSL only), like McpSceneControl.h, so the
// MCP unit tests can include it without an editor TU.

#include "MCP/McpSceneControl.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::MCP::AssetOpen
{
    using Json = nlohmann::json;

    // Frames the tool waits for a deferred load to land before it reports the
    // panel's state as final. The slowest route (Sound Graph) needs two frames
    // after its window first exists; the rest is slack for a throttled frame.
    inline constexpr int s_MaxSettleFrames = 30;

    // An asset handle arrives as a decimal string (u64 does not survive a JSON
    // double, and every other tool prints handles as strings) or as a JSON integer.
    [[nodiscard]] inline std::optional<u64> ParseHandle(const Json& value)
    {
        if (value.is_number_unsigned())
            return value.get<u64>();
        if (value.is_number_integer())
        {
            const auto signedValue = value.get<i64>();
            return signedValue >= 0 ? std::optional<u64>(static_cast<u64>(signedValue)) : std::nullopt;
        }
        if (value.is_string())
        {
            const auto text = value.get<std::string>();
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error == std::errc{} && end == text.data() + text.size() && !text.empty())
                return parsed;
        }
        return std::nullopt;
    }

    // Exactly one of `handle` / `path`. Returns a human-readable error, or nullopt.
    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, McpAssetOpenRequest& out)
    {
        const bool hasHandle = args.contains("handle");
        const bool hasPath = args.contains("path");
        if (hasHandle == hasPath)
            return "Pass exactly one of 'handle' (an asset handle) or 'path' (a file under the project's asset directory).";

        if (hasHandle)
        {
            const auto handle = ParseHandle(args["handle"]);
            if (!handle || *handle == 0)
                return "Invalid 'handle': expected a non-zero asset handle (a decimal string such as \"1234567890\").";
            out.Handle = *handle;
        }
        else
        {
            if (!args["path"].is_string() || args["path"].get<std::string>().empty())
                return "Invalid 'path': expected a non-empty file path.";
            out.Path = args["path"].get<std::string>();
            // The editor also checks containment on the final, canonical path;
            // this refuses the obvious case before anything touches the disk.
            if (SceneControl::HasParentTraversal(out.Path))
                return "Invalid 'path': parent-directory traversal ('..') is not allowed.";
        }

        out.DiscardUnsaved = args.value("discardUnsaved", false);
        return std::nullopt;
    }

    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("handle", Schema::Raw(Json{ { "type", Json::array({ "string", "integer" }) } })
                                .Desc("Asset handle (decimal string, as olo_asset_* tools print it). Pass this OR 'path'."))
            .Prop("path", Schema::String().Desc("File to open. Relative paths resolve against the project's asset directory "
                                                "(e.g. \"SoundGraphs/HelloDing.olosoundgraph\"); an absolute path must still lie "
                                                "inside it. '..' is rejected. Pass this OR 'handle'."))
            .Prop("discardUnsaved", Schema::Bool().Desc("The target editor (or, for a scene, the current scene) has unsaved "
                                                        "changes: false (default) refuses and says so, true discards them. The "
                                                        "Content Browser asks with a native Yes/No/Cancel dialog here, which an "
                                                        "agent cannot answer, so the choice is an argument instead."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json OutputSchema()
    {
        return Schema::Object()
            .Prop("ok", Schema::Bool().Desc("True only when the target panel is open AND reports the requested file as the one it has loaded."))
            .Prop("path", Schema::String().Desc("The absolute file path the request resolved to."))
            .Prop("handle", Schema::String().Desc("The asset handle, when the registry knows the file; \"0\" otherwise."))
            .Prop("fileType", Schema::String().Desc("The Content Browser's classification of the file (SoundGraph, SkillTree, VisualScript, Scene, ...)."))
            .Prop("panel", Schema::String().Desc("The editor panel that opened it (a name from olo_editor_panel_list), or \"scene\" for a scene."))
            .Prop("panelOpen", Schema::Bool())
            .Prop("loadedPath", Schema::String().Desc("What the panel reports as its loaded file after the wait. Empty when nothing is loaded."))
            .Prop("framesWaited", Schema::Int().Min(0))
            .Prop("message", Schema::String())
            .Required({ "ok", "path", "fileType", "panel", "panelOpen", "loadedPath", "message" });
    }

    // Final reply: the dispatch result plus the panel's own readback.
    [[nodiscard]] inline Json ToJson(const McpAssetOpenResult& opened, const McpAssetEditorState& state, int framesWaited,
                                     bool timedOut)
    {
        const bool ok = state.PanelOpen && state.Matches;
        std::string message;
        if (ok)
            message = "Opened " + opened.ResolvedPath + " in " + opened.Panel + ".";
        else if (!state.PanelOpen)
            message = "The " + opened.Panel + " panel is not open after the request; the file was not shown.";
        else if (state.LoadedPath.empty())
            message = "The " + opened.Panel + " panel is open but has no file loaded" +
                      (timedOut ? " after " + std::to_string(framesWaited) + " frames" : std::string{}) +
                      ". The load most likely failed to parse — check olo_log_tail.";
        else
            message = "The " + opened.Panel + " panel still shows " + state.LoadedPath + ", not the requested file" +
                      (timedOut ? " after " + std::to_string(framesWaited) + " frames" : std::string{}) +
                      ". The load most likely failed — check olo_log_tail.";

        return Json{ { "ok", ok },
                     { "path", opened.ResolvedPath },
                     { "handle", std::to_string(opened.Handle) },
                     { "fileType", opened.FileType },
                     { "panel", opened.Panel },
                     { "panelOpen", state.PanelOpen },
                     { "loadedPath", state.LoadedPath },
                     { "framesWaited", framesWaited },
                     { "message", message } };
    }
} // namespace OloEngine::MCP::AssetOpen
