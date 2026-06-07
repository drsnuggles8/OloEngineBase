#pragma once

namespace OloEngine::MCP
{
    class McpServer;

    // Register the read-only diagnostic tools onto `server`. Phase 0/1 surface:
    //   * olo_log_tail      — recent engine log lines (lock-safe)
    //   * olo_scene_summary — active-scene overview (main-marshaled)
    // Call once after constructing the server, before Start().
    void RegisterBuiltinTools(McpServer& server);
} // namespace OloEngine::MCP
