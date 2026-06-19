#pragma once

// A small, thread-safe ring buffer of recent script (C# / Lua) exceptions, mirroring
// the engine log ring buffer (Core/Log.h). Both script engines push to it from the
// game thread; the read-only MCP diagnostics server (#285) reads it from its handler
// thread, so all access is mutex-guarded.
//
// Header-only with an inline singleton: OloEngine is a static library, so the single
// function-local static is shared across every translation unit in the final binary.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine
{
    struct ScriptError
    {
        enum class Language : u8
        {
            CSharp = 0,
            Lua = 1
        };

        Language Lang = Language::CSharp;
        std::string ScriptName; // C#: full method/type name; Lua: script path (may be empty)
        u64 EntityId = 0;       // 0 = unknown / not available at the catch site
        std::string Message;    // exception text (Lua's includes a traceback)
        std::string StackTrace; // optional; may be empty when embedded in Message
        f64 Timestamp = 0.0;    // Unix epoch seconds, stamped on Push

        [[nodiscard]] static const char* LanguageString(Language language)
        {
            return language == Language::Lua ? "lua" : "csharp";
        }
    };

    class ScriptErrorBuffer
    {
      public:
        static ScriptErrorBuffer& Get()
        {
            static ScriptErrorBuffer s_Instance;
            return s_Instance;
        }

        void Push(ScriptError error)
        {
            error.Timestamp = std::chrono::duration<f64>(std::chrono::system_clock::now().time_since_epoch()).count();

            // Mirror every script error into the unified diagnostics timeline (#306
            // item B) so olo_events_tail shows scripting failures inline with scene/
            // play/spawn events; olo_script_get_last_errors still has the full detail
            // (stack trace). Summarise to the first line — Lua messages embed a traceback.
            {
                const std::string summary = error.Message.substr(0, error.Message.find('\n'));
                DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::ScriptError,
                                                  std::string("[") + ScriptError::LanguageString(error.Lang) + "] " + summary,
                                                  error.EntityId, error.ScriptName);
            }

            std::lock_guard lock(m_Mutex);
            m_Errors.push_back(std::move(error));
            while (m_Errors.size() > kCapacity)
                m_Errors.pop_front();
        }

        // Most recent `count` errors (oldest-first). count == 0 returns all buffered.
        [[nodiscard]] std::vector<ScriptError> GetRecent(std::size_t count = 0) const
        {
            std::lock_guard lock(m_Mutex);
            if (count == 0 || count >= m_Errors.size())
                return std::vector<ScriptError>(m_Errors.begin(), m_Errors.end());
            return std::vector<ScriptError>(m_Errors.end() - static_cast<std::ptrdiff_t>(count), m_Errors.end());
        }

        void Clear()
        {
            std::lock_guard lock(m_Mutex);
            m_Errors.clear();
        }

      private:
        ScriptErrorBuffer() = default;

        static constexpr std::size_t kCapacity = 64;
        mutable std::mutex m_Mutex;
        std::deque<ScriptError> m_Errors;
    };
} // namespace OloEngine
