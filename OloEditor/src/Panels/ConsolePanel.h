#pragma once

#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/Mutex.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace OloEngine
{
    // The editor's console: the log tail it always was, plus — since issue #821
    // — a command line that reads and writes console variables while the editor
    // keeps running.
    //
    // The loop this exists for: the editor is up showing the bug, you want to
    // flip OLO_RG_POISON_TRANSIENTS and look again. Before this that meant
    // exporting a variable and restarting, which loses the repro.
    class ConsolePanel
    {
      public:
        ConsolePanel();
        ~ConsolePanel();

        void OnImGuiRender(bool* p_open = nullptr);
        void Clear();

      private:
        struct LogEntry
        {
            std::string Message;
            Log::Level Level = Log::Level::Trace;
            Log::Type Source = Log::Type::Core; // Core vs Client logger
        };

        void PushMessage(const std::string& message, Log::Level level, Log::Type source);

        // --- Command line ---------------------------------------------------
        // Echoed straight into the entry ring so a command and its result read
        // in the same order as the log around them.
        void Echo(const std::string& text, Log::Level level = Log::Level::Info);
        void ExecuteCommand(const std::string& command);
        void PrintCVar(std::string_view name);
        void ListCVars(std::string_view filter);

        // ImGui's completion/history callback trampoline.
        static int InputTextCallback(ImGuiInputTextCallbackData* data);
        void OnCompletion(ImGuiInputTextCallbackData* data);
        void OnHistory(ImGuiInputTextCallbackData* data);

        char m_CommandBuffer[512] = {};
        // Bounded: an editor session runs for hours, and an unbounded history is
        // a slow leak for no benefit — nobody arrows back past a few dozen.
        static constexpr sizet s_MaxHistory = 100;
        std::vector<std::string> m_History;
        // -1 = not browsing history; otherwise an index into m_History.
        int m_HistoryPos = -1;
        bool m_ScrollToBottom = false;
        bool m_ReclaimFocus = false;

        // Ring buffer of log entries
        static constexpr size_t s_MaxEntries = 2048;
        std::deque<LogEntry> m_Entries;
        mutable FMutex m_Mutex;

        // Filter state
        bool m_ShowTrace = true;
        bool m_ShowInfo = true;
        bool m_ShowWarn = true;
        bool m_ShowError = true;
        bool m_AutoScroll = true;
        char m_FilterText[256] = {};

        // Sink handle for cleanup
        spdlog::sink_ptr m_Sink;

        void InstallSink();
        void RemoveSink();
    };
} // namespace OloEngine
