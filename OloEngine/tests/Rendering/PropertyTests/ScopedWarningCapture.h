#pragma once

// Every warning line the core logger emits while a ScopedWarningCapture is
// alive. A private sink rather than Log's shared 200-entry ring: a frame of the
// full renderer can push the line out of that one before it is read.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    class ScopedWarningCapture
    {
      public:
        ScopedWarningCapture()
        {
            m_Sink->set_level(spdlog::level::warn);
            m_Sink->set_pattern("%v");
            Log::Get().GetCoreLogger()->sinks().push_back(m_Sink);
        }
        ~ScopedWarningCapture()
        {
            auto& sinks = Log::Get().GetCoreLogger()->sinks();
            std::erase_if(sinks, [this](const spdlog::sink_ptr& sink)
                          { return sink == m_Sink; });
        }
        ScopedWarningCapture(const ScopedWarningCapture&) = delete;
        auto operator=(const ScopedWarningCapture&) -> ScopedWarningCapture& = delete;

        [[nodiscard]] u32 Count(std::string_view marker) const
        {
            u32 count = 0;
            for (const auto& line : m_Sink->last_formatted())
            {
                if (line.find(marker) != std::string::npos)
                    ++count;
            }
            return count;
        }

      private:
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> m_Sink =
            std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4096);
    };
} // namespace OloEngine::Tests
