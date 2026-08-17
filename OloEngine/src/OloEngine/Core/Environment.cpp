#include "OloEnginePCH.h"
#include "OloEngine/Core/Environment.h"

#include <charconv>
#include <cstdlib>

namespace OloEngine::Env
{
    std::optional<std::string> Get(const char* name)
    {
        if (name == nullptr || *name == '\0')
        {
            return std::nullopt;
        }

        // NOSONAR cpp:S990 — this is the engine's single getenv call site, and
        // the rule has no portable fix: it flags getenv because the returned
        // pointer can be invalidated by a concurrent setenv/putenv, and C++ has
        // no thread-safe alternative (Windows offers GetEnvironmentVariableA;
        // POSIX offers nothing, so any cross-platform helper bottoms out here).
        //
        // Why it is nonetheless safe: the value is copied into a std::string
        // before this function returns, so no caller ever holds the pointer.
        // The only writers in the repo are in the test harness
        // (OloEngineTest.cpp's main, McpDispatchTest's scoped setter), and the
        // header's contract asks callers to read once at startup, so a read
        // never overlaps one of those writes.
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
        {
            return std::nullopt;
        }
        return std::string(value);
    }

    bool IsTruthy(const char* name)
    {
        const std::optional<std::string> value = Get(name);
        if (!value)
        {
            return false;
        }
        const char first = value->front();
        return first != '0' && first != 'f' && first != 'F';
    }

    bool IsExactly(const char* name, std::string_view expected)
    {
        const std::optional<std::string> value = Get(name);
        return value && *value == expected;
    }

    std::optional<i64> GetInt(const char* name)
    {
        const std::optional<std::string> value = Get(name);
        if (!value)
        {
            return std::nullopt;
        }

        // from_chars rather than atoi: atoi returns 0 for unparseable input, so
        // a mistyped value silently becomes a legitimate-looking zero.
        i64 result = 0;
        const char* begin = value->data();
        const char* end = begin + value->size();
        const auto [ptr, ec] = std::from_chars(begin, end, result);
        if (ec != std::errc{} || ptr != end)
        {
            return std::nullopt;
        }
        return result;
    }
} // namespace OloEngine::Env
