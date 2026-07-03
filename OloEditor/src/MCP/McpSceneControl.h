#pragma once

// Shared, editor-side schema + result shaping for the consented MCP scene-control
// write tools (issue #316 Part 5): `olo_scene_open` (open/switch the active scene),
// `olo_scene_play` and `olo_scene_stop` (toggle Play mode). Together they give an
// agent scriptable control over which scene is loaded and whether the runtime is
// simulating — the "scriptable repro setup" the read-only server couldn't drive
// (previously a manual Sandbox.oloproj StartScene edit + editor relaunch per scene,
// and an OLO_EDITOR_AUTOPLAY=1 relaunch to reach Play).
//
// Like McpReloadScript.h, the ACTIONS cannot live in this header — opening a scene
// and toggling Play touch the EnTT registry / renderer / runtime, which a unit test
// must not invoke. So they are routed through the EditorMcpContext hooks
// (OpenSceneFromMcp / SetScenePlayState): the editor wires them to its Open Scene /
// OnScenePlay / OnSceneStop paths, a headless host leaves them null ("not
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
#include <cctype>
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
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext;
    }

    // Validate a scene path argument for `olo_scene_open`. Pure: it enforces only
    // what can be checked without the filesystem — a non-empty string with a
    // recognised scene extension (.olo / .scene) and no parent-directory traversal
    // (a ".." component would let a consented write escape the project tree). The
    // editor hook does the rest (resolve relative to the asset directory, check the
    // file exists, deserialize). Returns a human-readable error, or nullopt on OK.
    [[nodiscard]] inline std::optional<std::string> ValidateScenePath(std::string_view path)
    {
        if (path.empty())
            return "Missing required argument 'path' (a .olo or .scene scene file, relative to the project asset directory).";

        // Reject a ".." path component (traversal out of the project). Check between
        // separators so a filename that merely contains ".." (e.g. "a..b.olo") is fine.
        sizet start = 0;
        while (start <= path.size())
        {
            sizet sep = path.find_first_of("/\\", start);
            const std::string_view comp = path.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
            if (comp == "..")
                return "Invalid 'path': parent-directory traversal ('..') is not allowed.";
            if (sep == std::string_view::npos)
                break;
            start = sep + 1;
        }

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
            .Required({ "path" })
            .NoAdditional();
    }

    // inputSchema for olo_scene_play / olo_scene_stop: no arguments (the transition
    // direction is fixed by which tool you call, mirroring the editor's Play / Stop
    // buttons).
    [[nodiscard]] inline Json PlayStopInputSchema()
    {
        return Schema::EmptyObject();
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
            { "message", result.Message },
        };
    }

    [[nodiscard]] inline Json ToJson(const McpScenePlayResult& result)
    {
        return Json{
            { "available", result.Available },
            { "ok", result.Ok },
            { "playing", result.Playing },
            { "changed", result.Changed },
            { "sceneName", result.SceneName },
            { "message", result.Message },
        };
    }
} // namespace OloEngine::MCP::SceneControl
