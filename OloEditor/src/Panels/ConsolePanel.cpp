#include "OloEnginePCH.h"
#include "ConsolePanel.h"
#include "OloEngine/Core/CVar.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <imgui.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/details/log_msg_buffer.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    static Log::Level SpdlogLevelToOlo(spdlog::level::level_enum level)
    {
        switch (level)
        {
            case spdlog::level::trace:
                return Log::Level::Trace;
            case spdlog::level::debug:
            case spdlog::level::info:
                return Log::Level::Info;
            case spdlog::level::warn:
                return Log::Level::Warn;
            case spdlog::level::err:
                return Log::Level::Error;
            case spdlog::level::critical:
                return Log::Level::Fatal;
            default:
                return Log::Level::Trace;
        }
    }

    ConsolePanel::ConsolePanel()
    {
        InstallSink();
    }

    ConsolePanel::~ConsolePanel()
    {
        RemoveSink();
    }

    void ConsolePanel::InstallSink()
    {
        m_Sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& msg)
            {
                auto level = SpdlogLevelToOlo(msg.level);
                std::string payload(msg.payload.data(), msg.payload.size());
                std::string loggerName(msg.logger_name.data(), msg.logger_name.size());

                auto source = Log::Type::Core;
                if (loggerName == "APP")
                {
                    source = Log::Type::Client;
                }

                // Format: [LoggerName] message
                std::string formatted = fmt::format("[{}] {}", loggerName, payload);
                PushMessage(formatted, level, source);
            });

        // Add sink to core, client, and editor console loggers
        Log::Get().GetCoreLogger()->sinks().push_back(m_Sink);
        Log::Get().GetClientLogger()->sinks().push_back(m_Sink);
        Log::Get().GetEditorConsoleLogger()->sinks().push_back(m_Sink);
    }

    void ConsolePanel::RemoveSink()
    {
        if (!m_Sink)
        {
            return;
        }

        auto removeSinkFrom = [this](const std::shared_ptr<spdlog::logger>& logger)
        {
            auto& sinks = logger->sinks();
            std::erase(sinks, m_Sink);
        };

        removeSinkFrom(Log::Get().GetCoreLogger());
        removeSinkFrom(Log::Get().GetClientLogger());
        removeSinkFrom(Log::Get().GetEditorConsoleLogger());
        m_Sink.reset();
    }

    void ConsolePanel::PushMessage(const std::string& message, Log::Level level, Log::Type source)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (m_Entries.size() >= s_MaxEntries)
        {
            m_Entries.pop_front();
        }
        m_Entries.push_back({ message, level, source });
    }

    void ConsolePanel::Clear()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_Entries.clear();
    }

    // --- Command line -------------------------------------------------------

    namespace
    {
        // Both the whitespace rule and the type vocabulary come from CVars::,
        // so the console cannot accept different input, or name a type
        // differently, from --set and olo_cvar_set.
        using CVars::CVarTypeName;
        using CVars::TrimText;

        [[nodiscard]] std::string_view Trim(std::string_view text)
        {
            return TrimText(text);
        }
    } // namespace

    void ConsolePanel::Echo(const std::string& text, Log::Level level)
    {
        PushMessage(text, level, Log::Type::Client);
        m_ScrollToBottom = true;
    }

    void ConsolePanel::PrintCVar(std::string_view name)
    {
        const std::optional<CVars::CVarInfo> info = CVars::Find(name);
        if (!info)
        {
            Echo("unknown console variable '" + std::string(name) + "' - try 'list " + std::string(name) + "'",
                 Log::Level::Warn);
            return;
        }
        Echo(std::string(info->Name) + " = " + info->Value + "  (" + std::string(CVarTypeName(info->Type)) +
             (info->ReadOnly ? ", read-only" : "") + (info->IsDefault ? ", default" : "") + ")");
        Echo("    " + std::string(info->Help));
    }

    void ConsolePanel::ListCVars(std::string_view filter)
    {
        // Every registered name is upper-case OLO_*, so upper-casing the needle
        // once is the whole of case-insensitive matching here.
        std::string needle(Trim(filter));
        std::ranges::transform(needle, needle.begin(),
                               [](char c)
                               { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });

        u32 shown = 0;
        for (const CVars::CVarInfo& info : CVars::Snapshot())
        {
            if (!needle.empty() && info.Name.find(needle) == std::string_view::npos)
            {
                continue;
            }
            Echo(std::string(info.Name) + " = " + info.Value + (info.IsDefault ? "" : "   <-- not default"));
            ++shown;
        }
        Echo(std::to_string(shown) + " console variable(s)" +
             (needle.empty() ? std::string() : " matching '" + needle + "'"));
    }

    void ConsolePanel::ExecuteCommand(const std::string& command)
    {
        const std::string_view line = Trim(command);
        if (line.empty())
        {
            return;
        }

        Echo("> " + std::string(line));

        // Split into the first token and the rest. `NAME=VALUE` is accepted as
        // well as `NAME VALUE`, because both are what people type and the
        // `--set` form is the first one.
        std::string_view name = line;
        std::string_view value;
        bool hasValue = false;
        if (const sizet space = line.find_first_of(" \t"); space != std::string_view::npos)
        {
            name = Trim(line.substr(0, space));
            value = Trim(line.substr(space + 1));
            hasValue = true;
        }
        if (const sizet equals = name.find('='); equals != std::string_view::npos)
        {
            // `NAME=VALUE rest` would be ambiguous; take everything after the
            // '=' on the original line so quoted paths survive.
            const sizet lineEquals = line.find('=');
            name = Trim(line.substr(0, lineEquals));
            value = Trim(line.substr(lineEquals + 1));
            hasValue = true;
        }

        if (name == "help" || name == "?")
        {
            Echo("Console commands:");
            Echo("  <NAME>                 print a console variable's value and help");
            Echo("  <NAME> <VALUE>         set it (also accepts <NAME>=<VALUE>)");
            Echo("  list [TEXT]            list console variables, optionally filtered");
            Echo("  clear                  clear this panel");
            Echo("  help                   this text");
            Echo("Tab completes a name; Up/Down walk the command history. Booleans take "
                 "on/off (also 1/0, true/false, yes/no); an int or float also takes 'unset'.");
            Echo("The same names work as --set NAME=VALUE on the command line.");
            return;
        }
        if (name == "clear")
        {
            Clear();
            return;
        }
        if (name == "list")
        {
            ListCVars(hasValue ? value : std::string_view{});
            return;
        }

        if (!hasValue)
        {
            PrintCVar(name);
            return;
        }

        const CVars::SetResult result = CVars::SetFromString(name, value);
        if (!result.Ok)
        {
            Echo(result.Error, Log::Level::Warn);
            return;
        }
        if (!result.Changed)
        {
            Echo(std::string(name) + " = " + result.NewValue + " (unchanged)");
            return;
        }
        // The change reaches its observers at the top of the NEXT frame, not
        // here — say so, because "I set it and the frame still looks the same"
        // is otherwise indistinguishable from "the write did not work".
        Echo(std::string(name) + " = " + result.NewValue + "  (was " + result.OldValue +
             "; takes effect from the next frame)");
    }

    int ConsolePanel::InputTextCallback(ImGuiInputTextCallbackData* data)
    {
        auto* self = static_cast<ConsolePanel*>(data->UserData);
        switch (data->EventFlag)
        {
            case ImGuiInputTextFlags_CallbackCompletion:
                self->OnCompletion(data);
                break;
            case ImGuiInputTextFlags_CallbackHistory:
                self->OnHistory(data);
                break;
            default:
                break;
        }
        return 0;
    }

    void ConsolePanel::OnCompletion(ImGuiInputTextCallbackData* data)
    {
        // Complete the FIRST token only — the rest is a value, and cvar names
        // are the only thing there is a list of.
        const std::string_view text(data->Buf, static_cast<sizet>(data->CursorPos));
        if (text.find_first_of(" \t=") != std::string_view::npos)
        {
            return;
        }

        const std::vector<std::string_view> matches = CVars::Complete(text);
        if (matches.empty())
        {
            Echo("no console variable starts with '" + std::string(text) + "'", Log::Level::Warn);
            return;
        }

        // Extend as far as it is unambiguous, then list what is left. One match
        // completes fully and appends a space, which is what a shell does.
        const std::string completion = CVars::LongestCompletion(text);
        if (completion.size() > text.size() || matches.size() == 1)
        {
            // Replace the FIRST TOKEN only, not the whole buffer. With the caret
            // inside the name of `OLO_NAME value`, deleting BufTextLen would
            // silently eat the ` value` the user had already typed.
            const std::string_view whole(data->Buf, static_cast<sizet>(data->BufTextLen));
            const sizet tokenEnd = whole.find_first_of(" \t=");
            const int replaceLen =
                tokenEnd == std::string_view::npos ? data->BufTextLen : static_cast<int>(tokenEnd);

            data->DeleteChars(0, replaceLen);
            data->InsertChars(0, completion.c_str());
            // A trailing space only when there is nothing after the name yet —
            // otherwise it would be inserted in front of the existing value.
            if (matches.size() == 1 && tokenEnd == std::string_view::npos)
            {
                data->InsertChars(data->CursorPos, " ");
            }
        }

        if (matches.size() > 1)
        {
            for (const std::string_view match : matches)
            {
                Echo("  " + std::string(match));
            }
        }
    }

    void ConsolePanel::OnHistory(ImGuiInputTextCallbackData* data)
    {
        if (m_History.empty())
        {
            return;
        }

        const int previous = m_HistoryPos;
        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (m_HistoryPos == -1)
            {
                m_HistoryPos = static_cast<int>(m_History.size()) - 1;
            }
            else if (m_HistoryPos > 0)
            {
                --m_HistoryPos;
            }
        }
        else if (data->EventKey == ImGuiKey_DownArrow && m_HistoryPos != -1)
        {
            if (++m_HistoryPos >= static_cast<int>(m_History.size()))
            {
                m_HistoryPos = -1;
            }
        }

        if (previous == m_HistoryPos)
        {
            return;
        }
        const std::string replacement = m_HistoryPos >= 0 ? m_History[static_cast<sizet>(m_HistoryPos)] : std::string();
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, replacement.c_str());
    }

    void ConsolePanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::Begin("Console", p_open))
        {
            ImGui::End();
            return;
        }

        // Toolbar row
        if (ImGui::Button("Clear"))
        {
            Clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_AutoScroll);
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();

        // Severity filter toggles with color hints
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Checkbox("Trace", &m_ShowTrace);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
        ImGui::Checkbox("Info", &m_ShowInfo);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
        ImGui::Checkbox("Warn", &m_ShowWarn);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        ImGui::Checkbox("Error", &m_ShowError);
        ImGui::PopStyleColor();

        // Text filter
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##ConsoleFilter", "Filter...", m_FilterText, sizeof(m_FilterText));

        ImGui::Separator();

        // Log entries. Height leaves room for the command line below — without
        // the reservation the child eats the window and the input is unreachable.
        const f32 commandLineHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("LogEntries", ImVec2(0, -commandLineHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            TUniqueLock<FMutex> lock(m_Mutex);

            std::string_view filterStr(m_FilterText);

            for (const auto& entry : m_Entries)
            {
                // Severity filter
                switch (entry.Level)
                {
                    case Log::Level::Trace:
                        if (!m_ShowTrace)
                            continue;
                        break;
                    case Log::Level::Info:
                        if (!m_ShowInfo)
                            continue;
                        break;
                    case Log::Level::Warn:
                        if (!m_ShowWarn)
                            continue;
                        break;
                    case Log::Level::Error:
                    case Log::Level::Fatal:
                        if (!m_ShowError)
                            continue;
                        break;
                }

                // Text filter
                if (!filterStr.empty() && entry.Message.find(filterStr) == std::string::npos)
                {
                    continue;
                }

                // Color by severity
                ImVec4 color;
                switch (entry.Level)
                {
                    case Log::Level::Trace:
                        color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                        break;
                    case Log::Level::Info:
                        color = ImVec4(0.0f, 0.8f, 0.0f, 1.0f);
                        break;
                    case Log::Level::Warn:
                        color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                        break;
                    case Log::Level::Error:
                        color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                        break;
                    case Log::Level::Fatal:
                        color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                        break;
                    default:
                        color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                        break;
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(entry.Message.c_str());
                ImGui::PopStyleColor();
            }
        }

        if (m_ScrollToBottom || (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
        {
            ImGui::SetScrollHereY(1.0f);
        }
        m_ScrollToBottom = false;

        ImGui::EndChild();

        // Command line
        ImGui::Separator();
        constexpr ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                                   ImGuiInputTextFlags_CallbackCompletion |
                                                   ImGuiInputTextFlags_CallbackHistory;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##ConsoleCommand", "Console variable, or 'help' (Tab completes)",
                                     m_CommandBuffer, sizeof(m_CommandBuffer), inputFlags, &InputTextCallback, this))
        {
            std::string command(m_CommandBuffer);
            m_CommandBuffer[0] = '\0';
            m_HistoryPos = -1;
            if (!Trim(command).empty())
            {
                // Consecutive duplicates are noise when walking back through it.
                if (m_History.empty() || m_History.back() != command)
                {
                    // Capped: an editor session runs for hours and this vector
                    // would otherwise only ever grow.
                    if (m_History.size() >= s_MaxHistory)
                    {
                        m_History.erase(m_History.begin());
                    }
                    m_History.push_back(command);
                }
                ExecuteCommand(command);
            }
            m_ReclaimFocus = true;
        }

        // Keep the caret in the box across a submit, so a sequence of commands
        // does not need a click between each.
        ImGui::SetItemDefaultFocus();
        if (m_ReclaimFocus)
        {
            ImGui::SetKeyboardFocusHere(-1);
            m_ReclaimFocus = false;
        }

        ImGui::End();
    }

} // namespace OloEngine
