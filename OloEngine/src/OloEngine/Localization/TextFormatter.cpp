#include "OloEnginePCH.h"
#include "OloEngine/Localization/TextFormatter.h"

#include <charconv>
#include <string_view>

namespace OloEngine
{
    namespace
    {
        // Pull the i-th `|`-separated form out of `forms`. If i is past the
        // end, the last form is returned. An empty forms string yields an
        // empty string.
        std::string SelectPluralForm(std::string_view forms, u32 index)
        {
            u32 currentIndex = 0;
            sizet start = 0;
            for (sizet i = 0; i <= forms.size(); ++i)
            {
                if (i == forms.size() || forms[i] == '|')
                {
                    if (currentIndex == index)
                        return std::string(forms.substr(start, i - start));
                    if (i == forms.size())
                        return std::string(forms.substr(start, i - start));
                    ++currentIndex;
                    start = i + 1;
                }
            }
            return std::string();
        }

        bool TryParseInt(std::string_view s, i32& out)
        {
            if (s.empty())
                return false;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
            return res.ec == std::errc{} && res.ptr == s.data() + s.size();
        }
    } // namespace

    std::string TextFormatter::Format(const std::string& pattern, const ParamMap& params, PluralRule rule)
    {
        std::string out;
        out.reserve(pattern.size() + 16);

        const sizet n = pattern.size();
        for (sizet i = 0; i < n; ++i)
        {
            const char c = pattern[i];

            if (c == '{')
            {
                // `{{` — emit a literal '{'
                if (i + 1 < n && pattern[i + 1] == '{')
                {
                    out.push_back('{');
                    ++i;
                    continue;
                }

                // Find matching '}'. If none, emit the rest verbatim so the
                // user can see the malformed token in the output.
                const sizet closing = pattern.find('}', i + 1);
                if (closing == std::string::npos)
                {
                    out.append(pattern, i, std::string::npos);
                    break;
                }

                const std::string_view token(pattern.data() + i + 1, closing - i - 1);
                const sizet colon = token.find(':');
                if (colon == std::string_view::npos)
                {
                    // Simple {name} substitution.
                    const std::string name(token);
                    auto it = params.find(name);
                    if (it != params.end())
                        out.append(it->second);
                    else
                        out.append(pattern, i, closing - i + 1); // leave the literal token visible
                }
                else
                {
                    const std::string_view name = token.substr(0, colon);
                    const std::string_view forms = token.substr(colon + 1);

                    auto it = params.find(std::string(name));
                    if (it == params.end())
                    {
                        out.append(pattern, i, closing - i + 1);
                    }
                    else
                    {
                        i32 count = 0;
                        if (!TryParseInt(it->second, count))
                        {
                            out.append(pattern, i, closing - i + 1);
                        }
                        else
                        {
                            const u32 idx = ResolvePluralIndex(rule, count);
                            out.append(SelectPluralForm(forms, idx));
                        }
                    }
                }

                i = closing;
            }
            else if (c == '}' && i + 1 < n && pattern[i + 1] == '}')
            {
                // `}}` — emit a literal '}'
                out.push_back('}');
                ++i;
            }
            else
            {
                out.push_back(c);
            }
        }

        return out;
    }

    std::string TextFormatter::FormatPlural(const std::string& pattern, const std::string& countParam, i32 count, ParamMap params, PluralRule rule)
    {
        params[countParam] = std::to_string(count);
        return Format(pattern, params, rule);
    }
} // namespace OloEngine
