#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;
} // namespace OloEngine::Automation

namespace OloEngine::MCP
{
    class McpServer;

    // Register every built-in COMMAND onto `registry` — the whole native surface
    // except the four discovery-gateway commands, which describe an MCP catalogue
    // and belong to the adapter (issue #1123). No server is involved, and none
    // needs to exist: this is the entry point a CLI (#1125) or a headless harness
    // uses to get the same commands the editor serves.
    void RegisterBuiltinCommands(Automation::AutomationRegistry& registry);

    // Register the built-in diagnostic + inspection tools onto `server`: the
    // read-only diagnostics from #285 (logs, scene/ECS, perf, memory, shaders,
    // assets, scripts, crashes, screenshot) and the Tier-0 rendering-dev
    // harness from #316 (camera control, viewport size override, render-target
    // capture). Call once after constructing the server, before Start().
    void RegisterBuiltinTools(McpServer& server);
} // namespace OloEngine::MCP
