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
// schema-enforced dispatch, tools/list / tools/search, annotations, icons.
// Handlers run in a DEDICATED, capability-stripped sol::state
// (base/math/string/table only; dofile/loadfile/load/require removed; print →
// engine log) with a per-state MEMORY QUOTA and a per-call time watchdog: pure
// Lua compute plus a bridge:
//
//   olo.call_tool(name, args) -- invoke another registered tool
//   olo.log(message)          -- write to the engine log
//
// TIERS (issue #607). A script tool declares its tier at registration:
//   * default (`writes` absent/false) — READ-ONLY: ProjectWrite = false,
//     readOnlyHint:true, and olo.call_tool refuses every ProjectWrite tool. It
//     cannot mutate anything, directly or transitively.
//   * `writes = true` — WRITE-TIER: ProjectWrite = true, readOnlyHint:false. It
//     goes through the SAME per-action write-consent gate as a native write tool
//     (Disabled -> refused / Prompt -> modal / Allow-all), and only then may its
//     handler reach ProjectWrite tools through olo.call_tool.
// Write authority is the EXECUTING tool's own declared tier — never inherited,
// never ambient (see the bridge rule in ADR 0005 §5).
//
// Loading may happen with the server STOPPED (editor init) or RUNNING (live
// reload): McpServer::ReplaceScriptTools publishes the new tool vector as an
// atomic copy-on-write swap and fires notifications/tools/list_changed, which
// is why the server advertises `listChanged:true`.

#include "OloEngine/Core/Base.h"

#include <chrono>
#include <cstddef>
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

    // Per-call wall-clock budget enforced by the Lua debug-hook watchdog.
    inline constexpr std::chrono::milliseconds kDefaultScriptToolsTimeBudget{ 10000 };

    // Per-STATE memory quota (issue #607): the ceiling on live bytes allocated by
    // the script-tools Lua state, on top of whatever the sandbox itself costs
    // (the quota is armed AFTER the libraries + bridge are built, so it can never
    // starve the interpreter's own boot). A custom lua_Alloc refuses any
    // allocation that would cross it, so Lua raises a clean "not enough memory"
    // error that surfaces as a ToolResult::Error — an allocation bomb
    // (`while true do t[#t+1] = string.rep('x', 1e6) end`) fails its own call and
    // leaves the editor standing, instead of OOM-ing the process.
    //
    // 64 MiB: comfortably more than any digest/aggregation tool this sandbox is
    // for (its whole API is JSON tables from other tools), small enough that a
    // runaway loop is caught in well under a second.
    inline constexpr std::size_t kDefaultScriptToolsMemoryBudget = 64ull * 1024 * 1024;

    // Scan `directory` for *.lua files (sorted, non-recursive), execute each in a
    // fresh sandboxed Lua state shared by the directory's tools, and publish every
    // valid RegisterMcpTool{...} call onto `server`. REPLACE semantics: the new
    // set of script tools atomically supersedes the previous one
    // (McpServer::ReplaceScriptTools), so a rescan picks up edits AND deletions.
    //
    // Safe to call with the server STOPPED (editor init / panel start) or RUNNING
    // (live reload, #607): the tool registry is a copy-on-write snapshot, so an
    // in-flight call keeps running against the tools — and the Lua state — it
    // started with, and the swap fires notifications/tools/list_changed.
    //
    // A missing directory is a clean no-op that still clears the previous script
    // tools (a deleted McpTools/ directory means "no script tools"). `budget` is
    // the per-call watchdog; `memoryBudgetBytes` is the per-state memory quota.
    McpScriptToolsReport LoadScriptTools(McpServer& server, const std::filesystem::path& directory,
                                         std::chrono::milliseconds budget = kDefaultScriptToolsTimeBudget,
                                         std::size_t memoryBudgetBytes = kDefaultScriptToolsMemoryBudget);

    // Register the native `olo_script_tools_reload` tool: rescans `directory` and
    // republishes the script tools LIVE (no server restart), returning the load
    // report. Not ProjectWrite — it mutates no project data; it re-executes the
    // project's own sandboxed Lua in the capability-stripped state, and any
    // write-tier tool it (re)registers still faces the write-consent gate on its
    // own dispatch, so a reload grants no authority the caller did not already
    // have. Call once, at editor init, alongside RegisterBuiltinTools.
    void RegisterScriptToolsReloadTool(McpServer& server, std::filesystem::path directory,
                                       std::chrono::milliseconds budget = kDefaultScriptToolsTimeBudget,
                                       std::size_t memoryBudgetBytes = kDefaultScriptToolsMemoryBudget);

    // The conventional per-project script-tools directory:
    // `<project assets>/McpTools`. Empty when no project is active (the caller
    // then skips loading).
    [[nodiscard]] std::filesystem::path DefaultScriptToolsDirectory();
} // namespace OloEngine::MCP
