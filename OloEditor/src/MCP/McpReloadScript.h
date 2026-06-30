#pragma once

// Shared, editor-side schema + result shaping for the consented MCP write tool
// `olo_reload_script` (issue #306 item C): reload the C# script assembly — the
// editor's Script ▸ Reload assembly (Ctrl+R) path, ScriptEngine::ReloadAssembly()
// — so an agent can iterate on freshly-built C# scripts over MCP without
// restarting the editor (the scripting counterpart of `olo_shader_reload`'s inner
// loop for shaders).
//
// Unlike the field-write tools (McpSetCollisionLayer.h / McpGenericFieldWrite.h),
// the reload ACTION cannot live in this header: it calls into ScriptEngine /Mono,
// which a unit test must not actually invoke. So the action is routed through the
// EditorMcpContext::ReloadScriptAssembly hook (the same indirection as
// GetCommandHistory) — the editor wires it to ScriptEngine::ReloadAssembly(), a
// headless host leaves it null ("not available"), and a test injects a fake. What
// stays here, header-only and engine-script-free, is the bit the test pins: the
// tool's inputSchema and the McpScriptReloadResult -> JSON shaping the handler
// emits.
//
// The reload is whole-assembly (C# reloads the entire app assembly; there is no
// per-script granularity in ScriptEngine), so the tool takes NO arguments — an
// empty-object schema, exactly mirroring the editor's parameterless Ctrl+R.
//
// Engine-script-free: only McpServer.h (the McpScriptReloadResult struct + Json)
// and the schema-builder DSL, so it pulls no extra editor TU into the MCP test
// binary (which deliberately does NOT link McpTools.cpp).

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <string>

namespace OloEngine::MCP::ReloadScript
{
    using Json = nlohmann::json;

    // The tool's inputSchema: no arguments (the C# reload is whole-assembly, like the
    // editor's parameterless Ctrl+R). The server validates against it before dispatch
    // (#423), so an unexpected property is rejected up front.
    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::EmptyObject();
    }

    // The structured result payload an agent sees, shaped from the hook's outcome.
    // `available` distinguishes "C# scripting is off in this build / not initialized"
    // (a clean, honest result) from a real reload; `ok` reports whether the reload
    // ran; `scriptClassCount` is a non-zero signal the app assembly loaded.
    [[nodiscard]] inline Json ToJson(const McpScriptReloadResult& result)
    {
        return Json{
            { "language", result.Language },
            { "available", result.Available },
            { "ok", result.Ok },
            { "scriptClassCount", result.ScriptClassCount },
            { "message", result.Message },
        };
    }
} // namespace OloEngine::MCP::ReloadScript
