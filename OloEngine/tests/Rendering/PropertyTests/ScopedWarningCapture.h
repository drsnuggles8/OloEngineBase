#pragma once

// Every warning line the core logger emits while a ScopedWarningCapture is
// alive. A private sink rather than Log's shared 200-entry ring: a frame of the
// full renderer can push the line out of that one before it is read.
//
// ONE sink, installed on the core logger once, before any test runs
// (InstallWarningCaptureSink, from the test binary's main), and never removed.
// A capture only switches it on and off, under the sink's own mutex -- the one
// spdlog holds while it writes to the sink. Adding and removing a sink per
// capture mutated spdlog's sink vector while the task scheduler's workers were
// logging through it: a data race ThreadSanitizer caught in GroomStreamingTest
// (#1559), latent in every test that used the capture beside the scheduler.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    class WarningCaptureSink final : public spdlog::sinks::base_sink<std::mutex>
    {
      public:
        void Begin()
        {
            std::lock_guard lock(mutex_);
            if (m_Depth++ == 0u)
            {
                m_Lines.clear();
            }
        }

        void End()
        {
            std::lock_guard lock(mutex_);
            m_Depth = m_Depth > 0u ? m_Depth - 1u : 0u;
        }

        [[nodiscard]] u32 Count(std::string_view marker)
        {
            std::lock_guard lock(mutex_);
            u32 count = 0;
            for (const std::string& line : m_Lines)
            {
                if (line.find(marker) != std::string::npos)
                    ++count;
            }
            return count;
        }

      protected:
        void sink_it_(const spdlog::details::log_msg& msg) override
        {
            if (m_Depth == 0u || msg.level < spdlog::level::warn)
            {
                return;
            }
            m_Lines.emplace_back(msg.payload.data(), msg.payload.size());
        }

        void flush_() override {}

      private:
        std::vector<std::string> m_Lines;
        u32 m_Depth = 0;
    };

    // The process's capture sink, added to the core logger on the first call.
    // The test binary's main calls this before any test (and any thread) runs,
    // so the sink vector is never changed while something logs.
    inline WarningCaptureSink& InstallWarningCaptureSink()
    {
        static const std::shared_ptr<WarningCaptureSink> s_Sink = []
        {
            auto sink = std::make_shared<WarningCaptureSink>();
            sink->set_level(spdlog::level::warn);
            Log::Get().GetCoreLogger()->sinks().push_back(sink);
            return sink;
        }();
        return *s_Sink;
    }

    class ScopedWarningCapture
    {
      public:
        ScopedWarningCapture()
            : m_Sink(InstallWarningCaptureSink())
        {
            m_Sink.Begin();
        }
        ~ScopedWarningCapture()
        {
            m_Sink.End();
        }
        ScopedWarningCapture(const ScopedWarningCapture&) = delete;
        auto operator=(const ScopedWarningCapture&) -> ScopedWarningCapture& = delete;

        [[nodiscard]] u32 Count(std::string_view marker) const
        {
            return m_Sink.Count(marker);
        }

      private:
        WarningCaptureSink& m_Sink;
    };
} // namespace OloEngine::Tests
