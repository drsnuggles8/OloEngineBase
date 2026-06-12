#pragma once

namespace OloEngine::MCP
{
    class McpServer;

    // Register the built-in diagnostic + inspection tools onto `server`: the
    // read-only diagnostics from #285 (logs, scene/ECS, perf, memory, shaders,
    // assets, scripts, crashes, screenshot) and the Tier-0 rendering-dev
    // harness from #316 (camera control, viewport size override, render-target
    // capture). Call once after constructing the server, before Start().
    void RegisterBuiltinTools(McpServer& server);
} // namespace OloEngine::MCP
