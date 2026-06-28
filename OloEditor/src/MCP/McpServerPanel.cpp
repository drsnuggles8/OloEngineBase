#include "OloEnginePCH.h"
#include "MCP/McpServerPanel.h"
#include "MCP/McpServer.h"

#include "OloEngine/Debug/Profiler.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>

namespace OloEngine::MCP
{
    void RenderMcpServerPanel(McpServer& server, int& port, bool& autoStart, bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::Begin("MCP Diagnostics Server", p_open))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
            "Expose this running editor's read-only diagnostics to your own MCP agent "
            "(Claude Code / Desktop). Binds 127.0.0.1 only, read-only, off by default. "
            "Anything with the token below can read your scene and logs, so share it only "
            "with your own agent.");
        ImGui::Separator();

        const bool running = server.IsRunning();

        // `port` / `autoStart` are persisted in EditorPreferences (owned by EditorLayer).
        static std::string s_StartError;

        if (running)
        {
            ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.30f, 1.0f), "Status: RUNNING");

            const std::string url = std::format("http://127.0.0.1:{}/mcp", server.GetPort());
            ImGui::Text("Endpoint:");
            ImGui::SameLine();
            ImGui::TextUnformatted(url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##url"))
                ImGui::SetClipboardText(url.c_str());

            ImGui::Spacing();
            ImGui::TextUnformatted("Auth token:");
            std::string token = server.GetToken();
            ImGui::PushItemWidth(-1.0f);
            ImGui::InputText("##token", token.data(), token.size() + 1, ImGuiInputTextFlags_ReadOnly);
            ImGui::PopItemWidth();
            if (ImGui::Button("Copy token"))
                ImGui::SetClipboardText(token.c_str());

            ImGui::Spacing();
            ImGui::TextUnformatted("One-line setup for Claude Code:");
            std::string command = std::format(
                "claude mcp add --transport http oloeditor {} --header \"Authorization: Bearer {}\"",
                url, token);
            ImGui::PushItemWidth(-1.0f);
            ImGui::InputText("##command", command.data(), command.size() + 1, ImGuiInputTextFlags_ReadOnly);
            ImGui::PopItemWidth();
            if (ImGui::Button("Copy command"))
                ImGui::SetClipboardText(command.c_str());

            ImGui::Spacing();
            if (const int streams = server.ActiveStreamCount(); streams > 0)
                ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.30f, 1.0f),
                                   "Live event push: %d stream(s) connected (GET /mcp SSE)", streams);
            else
                ImGui::TextDisabled("Live event push: idle (an agent's GET /mcp opens a live event stream)");

            ImGui::Separator();
            if (ImGui::Button("Stop server"))
                server.Stop();
        }
        else
        {
            ImGui::TextColored(ImVec4(0.85f, 0.60f, 0.30f, 1.0f), "Status: stopped");

            ImGui::PushItemWidth(120.0f);
            ImGui::InputInt("Port", &port);
            ImGui::PopItemWidth();
            port = std::clamp(port, 1024, 65535);

            if (ImGui::Button("Start server"))
            {
                s_StartError = server.Start(static_cast<u16>(port))
                                   ? std::string{}
                                   : std::format("Failed to start — could not bind 127.0.0.1:{} (port in use?).", port);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(binds 127.0.0.1 only)");

            if (!s_StartError.empty())
                ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.30f, 1.0f), "%s", s_StartError.c_str());
        }

        ImGui::Separator();
        if (bool redact = server.RedactPaths(); ImGui::Checkbox("Redact file paths in output", &redact))
            server.SetRedactPaths(redact);
        ImGui::SameLine();
        ImGui::TextDisabled("(scrubs absolute paths before they leave the process)");

        // Session write gate (issue #306 item C). OFF by default and not persisted, so
        // every launch starts read-only; the user opts in here for the session. While
        // off, any project-mutating tool (e.g. olo_set_collision_layer) is refused with
        // a clean JSON-RPC error even for an authenticated agent. Writes route through
        // the editor undo stack, so an agent's change is a single Ctrl-Z.
        if (bool allowWrites = server.AllowWrites(); ImGui::Checkbox("Allow writes (undoable)", &allowWrites))
            server.SetAllowWrites(allowWrites);
        ImGui::SameLine();
        if (server.AllowWrites())
            ImGui::TextColored(ImVec4(0.85f, 0.60f, 0.30f, 1.0f),
                               "(agents may MUTATE the scene via the undo stack — Ctrl-Z to revert)");
        else
            ImGui::TextDisabled("(off: write tools are refused; read-only. Not persisted — resets each launch)");

        ImGui::Checkbox("Start automatically when the editor launches", &autoStart);
        ImGui::SameLine();
        ImGui::TextDisabled("(persisted; default off)");

        ImGui::Text("Exposed (%s): %d tools, %d resources, %d prompts",
                    server.AllowWrites() ? "writes ON" : "read-only",
                    static_cast<int>(server.Tools().size()),
                    static_cast<int>(server.Resources().size()),
                    static_cast<int>(server.Prompts().size()));

        ImGui::End();
    }
} // namespace OloEngine::MCP
