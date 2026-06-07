#pragma once

namespace OloEngine::MCP
{
    class McpServer;

    // Renders the MCP server control window: start/stop, the localhost port, the
    // generated auth token, and a copy-paste `claude mcp add` connect command.
    // `port` and `autoStart` are edited in place and persisted by the caller
    // (EditorPreferences). `p_open` drives the window's visibility.
    void RenderMcpServerPanel(McpServer& server, int& port, bool& autoStart, bool* p_open);
} // namespace OloEngine::MCP
