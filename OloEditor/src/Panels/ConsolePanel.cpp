#include "ConsolePanel.h"
#include "OloEngine/Debug/Profiler.h"

#include <imgui.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/details/log_msg_buffer.h>

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

        // Add sink to both core and client loggers
        Log::GetCoreLogger()->sinks().push_back(m_Sink);
        Log::GetClientLogger()->sinks().push_back(m_Sink);
    }

    void ConsolePanel::RemoveSink()
    {
        if (!m_Sink)
        {
            return;
        }

        auto removeSinkFrom = [this](std::shared_ptr<spdlog::logger>& logger)
        {
            auto& sinks = logger->sinks();
            std::erase(sinks, m_Sink);
        };

        removeSinkFrom(Log::GetCoreLogger());
        removeSinkFrom(Log::GetClientLogger());
        m_Sink.reset();
    }

    void ConsolePanel::PushMessage(const std::string& message, Log::Level level, Log::Type source)
    {
        std::lock_guard lock(m_Mutex);
        if (m_Entries.size() >= s_MaxEntries)
        {
            m_Entries.pop_front();
        }
        m_Entries.push_back({ message, level, source });
    }

    void ConsolePanel::Clear()
    {
        std::lock_guard lock(m_Mutex);
        m_Entries.clear();
    }

    void ConsolePanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::Begin("Console"))
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

        // Log entries
        ImGui::BeginChild("LogEntries", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard lock(m_Mutex);

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

        if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

} // namespace OloEngine
