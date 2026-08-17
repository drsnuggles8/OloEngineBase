#include "OloEnginePCH.h"
#include "OloEngine/Core/Environment.h"

#include <charconv>
#include <cstdlib>

namespace OloEngine::Env
{
    std::optional<std::string> Get(std::string_view name)
    {
        if (name.empty())
        {
            return std::nullopt;
        }

        // The engine's single getenv call site — see sonar-project.properties'
        // env_s990 exclusion for why cpp:S990 doesn't apply here (a code
        // comment is not a valid suppression channel; that is itself a lesson
        // this call site's history taught).
        //
        // string_view isn't guaranteed null-terminated, so a copy is required
        // before it can reach getenv regardless of the S990 question.
        const std::string owned(name);
        const char* value = std::getenv(owned.c_str());
        if (value == nullptr || *value == '\0')
        {
            return std::nullopt;
        }
        return std::string(value);
    }

    bool IsTruthy(std::string_view name)
    {
        const std::optional<std::string> value = Get(name);
        if (!value)
        {
            return false;
        }
        const char first = value->front();
        return first != '0' && first != 'f' && first != 'F';
    }

    bool IsExactly(std::string_view name, std::string_view expected)
    {
        const std::optional<std::string> value = Get(name);
        return value && *value == expected;
    }

    std::optional<i64> GetInt(std::string_view name)
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
