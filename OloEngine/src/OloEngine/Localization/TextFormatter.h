#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Localization/LocaleDefinition.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace OloEngine
{
    // Parameter substitution + plural-form resolution for localized strings.
    //
    // Two token kinds are recognised inside the pattern:
    //   - {name}                  — replaced with params["name"]
    //   - {name:form0|form1|...}  — replaced with the form selected by the
    //     locale's plural rule applied to the integer value of params["name"]
    //
    // `{{` and `}}` produce literal `{` and `}`. Tokens that reference a
    // missing parameter, or whose embedded value is not a valid integer for
    // plural tokens, fall back to the raw token text so the missing data is
    // visible during testing rather than silently dropped.
    class TextFormatter
    {
      public:
        using ParamMap = std::unordered_map<std::string, std::string>;

        [[nodiscard("Store this!")]] static std::string Format(const std::string& pattern, const ParamMap& params, PluralRule rule);

        // Convenience overload — defaults to English-style one/other plurals.
        [[nodiscard("Store this!")]] static std::string Format(const std::string& pattern, const ParamMap& params)
        {
            return Format(pattern, params, PluralRule::OneOther);
        }

        // Inject `countParam = std::to_string(count)` into params (overwriting
        // any prior value) and Format(). Lets callers say
        // FormatPlural("kills", "count", 5, {}) without manually stringifying.
        [[nodiscard("Store this!")]] static std::string FormatPlural(const std::string& pattern, const std::string& countParam, i32 count, ParamMap params, PluralRule rule);

        [[nodiscard("Store this!")]] static std::string FormatPlural(const std::string& pattern, const std::string& countParam, i32 count, ParamMap params)
        {
            return FormatPlural(pattern, countParam, count, std::move(params), PluralRule::OneOther);
        }
    };
} // namespace OloEngine
