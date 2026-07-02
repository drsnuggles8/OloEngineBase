#pragma once

namespace OloEngine::MCP
{
    class McpServer;

    // Renders the MCP server control window: start/stop, the localhost port, the
    // generated auth token, and a copy-paste `claude mcp add` connect command.
    // `port` and `autoStart` are edited in place and persisted by the caller
    // (EditorPreferences). `p_open` drives the window's visibility.
    void RenderMcpServerPanel(McpServer& server, int& port, bool& autoStart, bool* p_open);

    // Renders the per-action write-consent modal (issue #306 item C). Call this every
    // frame, UNCONDITIONALLY — independent of whether the MCP panel window above is
    // open — so an agent's write in Prompt mode is never left blocked because the user
    // closed the panel. A no-op when nothing is awaiting consent (the common case).
    void RenderMcpConsentModal(McpServer& server);
} // namespace OloEngine::MCP
