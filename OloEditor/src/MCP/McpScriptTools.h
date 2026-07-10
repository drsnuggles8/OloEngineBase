#pragma once

// Script-defined MCP tools (issue #357, Lua v1; design in
// docs/adr/0005-mcp-script-tools-lua-sandbox.md).
//
// A game developer drops `*.lua` files into `<project assets>/McpTools/`; each
// file calls the injected global
//
//   RegisterMcpTool{ name = "script_...", description = "...", handler = fn,
//                    title = ?, toolset = ?, schema = ? }
//
// and the tool joins the server's surface exactly like a native tool —
// schema-enforced dispatch, tools/list / tools/search, annotations. Handlers
// run in a DEDICATED, capability-stripped sol::state (base/math/string/table
// only; dofile/loadfile/load/require removed; print → engine log): pure Lua
// compute plus a read-only bridge:
//
//   olo.call_tool(name, args) -- invoke a registered non-ProjectWrite tool
//   olo.log(message)          -- write to the engine log
//
// Script tools are read-only BY CONSTRUCTION (the bridge rejects ProjectWrite
// tools), so every one is registered with readOnlyHint:true and never touches
// the write-consent machinery. Loading happens before McpServer::Start() —
// the tool set is immutable per server run (reload = restart the server from
// the panel), keeping `listChanged:false` honest.

#include "OloEngine/Core/Base.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::MCP
{
    class McpServer;

    // Outcome of one LoadScriptTools scan, for the panel/log to surface.
    struct McpScriptToolsReport
    {
        int ToolsRegistered = 0;
        int FilesLoaded = 0;
        int Failures = 0; // files that failed to run + registrations rejected
        std::vector<std::string> Messages;
    };

    // Scan `directory` for *.lua files (sorted, non-recursive), execute each in
    // a fresh sandboxed Lua state shared by the directory's tools, and register
    // every valid RegisterMcpTool{...} call onto `server`. REPLACE semantics:
    // previously loaded script tools are unregistered first, so a rescan picks
    // up edits and deletions. Call only while the server is STOPPED (editor
    // init, or the panel's start button before Start()) — the tool vector must
    // not mutate while dispatch threads read it. A missing directory is a clean
    // no-op (0 loaded). `budget` is the per-call watchdog: a handler that runs
    // longer is errored out inside Lua (see the ADR's failure-containment
    // section); it also bounds how quickly a cancelled call is abandoned.
    McpScriptToolsReport LoadScriptTools(McpServer& server, const std::filesystem::path& directory,
                                         std::chrono::milliseconds budget = std::chrono::milliseconds(10000));

    // The conventional per-project script-tools directory:
    // `<project assets>/McpTools`. Empty when no project is active (the caller
    // then skips loading).
    [[nodiscard]] std::filesystem::path DefaultScriptToolsDirectory();
} // namespace OloEngine::MCP
