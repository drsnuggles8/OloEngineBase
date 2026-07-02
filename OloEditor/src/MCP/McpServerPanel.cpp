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

        // Session write consent (issue #306 item C). Disabled by default and not
        // persisted, so every launch starts read-only; the user opts in here for the
        // session. Three modes compose the session gate with the per-action dialog:
        //   Disabled  — project-mutating tools are refused with a clean JSON-RPC error.
        //   Prompt    — each write pops a modal the user Approves/Denies (see below).
        //   Allow all — writes auto-apply for the session, no prompt.
        // Writes route through the editor undo stack, so an agent's change is a Ctrl-Z.
        ImGui::TextUnformatted("Agent writes:");
        ImGui::SameLine();
        const WriteConsentMode mode = server.GetWriteConsentMode();
        if (ImGui::RadioButton("Disabled", mode == WriteConsentMode::Disabled))
            server.SetWriteConsentMode(WriteConsentMode::Disabled);
        ImGui::SameLine();
        if (ImGui::RadioButton("Prompt", mode == WriteConsentMode::Prompt))
            server.SetWriteConsentMode(WriteConsentMode::Prompt);
        ImGui::SameLine();
        if (ImGui::RadioButton("Allow all", mode == WriteConsentMode::AllowSession))
            server.SetWriteConsentMode(WriteConsentMode::AllowSession);

        switch (mode)
        {
            case WriteConsentMode::Disabled:
                ImGui::TextDisabled("(read-only: write tools are refused. Not persisted — resets each launch)");
                break;
            case WriteConsentMode::Prompt:
                ImGui::TextColored(ImVec4(0.85f, 0.60f, 0.30f, 1.0f),
                                   "(each agent write asks you to Approve/Deny — Ctrl-Z reverts an approved one)");
                break;
            case WriteConsentMode::AllowSession:
                ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.30f, 1.0f),
                                   "(agents may MUTATE the scene WITHOUT asking — Ctrl-Z to revert)");
                break;
            default:
                // Unknown / future mode: fail safe to a neutral note rather than an
                // unlabeled state (also satisfies -Wswitch-default / SonarQube S131).
                ImGui::TextDisabled("(unknown write-consent mode)");
                break;
        }

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

    void RenderMcpConsentModal(McpServer& server)
    {
        OLO_PROFILE_FUNCTION();

        // One prompt at a time (oldest first): a second concurrent write parks in the
        // queue and surfaces on the next frame once this one is resolved.
        const std::vector<McpServer::PendingConsent> pending = server.PendingConsents();
        if (pending.empty())
            return;
        const McpServer::PendingConsent& request = pending.front();

        constexpr const char* kPopupId = "MCP write request##mcp_consent";
        if (!ImGui::IsPopupOpen(kPopupId))
            ImGui::OpenPopup(kPopupId);

        // Center on first appearance; let the user drag it afterwards.
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.30f, 1.0f),
                               "An MCP agent wants to MODIFY your scene.");
            ImGui::Separator();

            ImGui::Text("Tool: %s", request.ToolTitle.c_str());
            if (request.ToolName != request.ToolTitle)
                ImGui::TextDisabled("(%s)", request.ToolName.c_str());

            ImGui::Spacing();
            ImGui::TextUnformatted("Requested change:");
            // Bound the (agent-supplied) summary: a long field value or many-line
            // argument list would otherwise widen/lengthen the AlwaysAutoResize popup
            // until the Approve/Deny buttons are pushed off-screen. Wrap it in a
            // fixed-width, height-capped scrolling child so the modal always fits.
            constexpr float kSummaryWidth = 440.0f;
            const float maxSummaryHeight = ImGui::GetTextLineHeightWithSpacing() * 10.0f;
            const ImVec2 summarySize =
                ImGui::CalcTextSize(request.Summary.c_str(), nullptr, false, kSummaryWidth);
            const float summaryHeight = std::min(summarySize.y, maxSummaryHeight) + ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::BeginChild("##mcp_consent_summary", ImVec2(kSummaryWidth, summaryHeight), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(request.Summary.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::TextDisabled("Approving applies the change through the undo stack (Ctrl-Z to revert).");
            if (const int extra = static_cast<int>(pending.size()) - 1; extra > 0)
                ImGui::TextDisabled("(%d more request%s waiting)", extra, extra == 1 ? "" : "s");
            ImGui::Separator();

            if (ImGui::Button("Approve"))
            {
                server.ResolveConsent(request.Id, ConsentDecision::Approve);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Deny"))
            {
                server.ResolveConsent(request.Id, ConsentDecision::Deny);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Approve all this session"))
            {
                server.ResolveConsent(request.Id, ConsentDecision::ApproveAll);
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
} // namespace OloEngine::MCP
