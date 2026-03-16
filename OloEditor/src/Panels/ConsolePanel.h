#pragma once

#include "OloEngine/Core/Log.h"

#include <deque>
#include <mutex>
#include <string>

namespace OloEngine
{
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

        // Ring buffer of log entries
        static constexpr size_t s_MaxEntries = 2048;
        std::deque<LogEntry> m_Entries;
        mutable std::mutex m_Mutex;

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
