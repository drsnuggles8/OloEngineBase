#pragma once

namespace OloEngine::MCP
{
    class McpServer;

    // Renders the MCP server control window: start/stop, the localhost port, the
    // generated auth token, and a copy-paste `claude mcp add` connect command.
    // `p_open` drives the window's visibility (toggled from the Window menu).
    void RenderMcpServerPanel(McpServer& server, bool* p_open);
}
