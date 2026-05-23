#include "OloEnginePCH.h"
#include "OloEngine/Localization/TextFormatter.h"

#include <charconv>
#include <optional>
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

        // Map a string value to a gender form-index when it names a known
        // category. Lets `{gender:le|la}` route to "le" when params["gender"]
        // is "masculine" / "M" / "m", and to "la" when it's "feminine" / "F"
        // / "f". Unrecognised values yield std::nullopt so the formatter
        // falls back to its "leave token literal" path.
        std::optional<u32> TryParseGender(std::string_view s) noexcept
        {
            if (s.empty())
                return std::nullopt;
            // Case-insensitive single-letter shorthand.
            if (s.size() == 1)
            {
                switch (s[0])
                {
                    case 'm':
                    case 'M':
                        return 0u;
                    case 'f':
                    case 'F':
                        return 1u;
                    case 'n':
                    case 'N':
                        return 2u;
                    default:
                        return std::nullopt;
                }
            }
            const auto eqi = [](std::string_view a, std::string_view b) noexcept -> bool
            {
                if (a.size() != b.size())
                    return false;
                for (sizet i = 0; i < a.size(); ++i)
                {
                    const char ac = a[i];
                    const char bc = b[i];
                    const char ai = (ac >= 'A' && ac <= 'Z') ? static_cast<char>(ac + 32) : ac;
                    const char bi = (bc >= 'A' && bc <= 'Z') ? static_cast<char>(bc + 32) : bc;
                    if (ai != bi)
                        return false;
                }
                return true;
            };
            if (eqi(s, "masculine") || eqi(s, "male"))
                return 0u;
            if (eqi(s, "feminine") || eqi(s, "female"))
                return 1u;
            if (eqi(s, "neuter") || eqi(s, "none") || eqi(s, "other"))
                return 2u;
            return std::nullopt;
        }

        // Labelled-form selection for the `select` token shape:
        //   {role:warrior=knight|mage=wizard|else=hero}
        //
        // Each segment is `<label>=<form>`. Returns the form whose label
        // matches `value` exactly. If no segment matches, looks for the
        // optional `else=<form>` fallback. Returns std::nullopt when there
        // is no match and no `else` clause, so the formatter can leave the
        // token literal — making author errors visible.
        std::optional<std::string> ResolveSelectForm(std::string_view forms, std::string_view value)
        {
            std::optional<std::string> elseForm;
            sizet start = 0;
            for (sizet i = 0; i <= forms.size(); ++i)
            {
                if (i == forms.size() || forms[i] == '|')
                {
                    const std::string_view segment = forms.substr(start, i - start);
                    const sizet eq = segment.find('=');
                    if (eq != std::string_view::npos)
                    {
                        const std::string_view label = segment.substr(0, eq);
                        const std::string_view form = segment.substr(eq + 1);
                        if (label == value)
                            return std::string(form);
                        if (label == "else")
                            elseForm = std::string(form);
                    }
                    start = i + 1;
                }
            }
            return elseForm;
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
                        // Disambiguate the form syntax. If any segment
                        // contains '=' it's a labelled `select` token
                        // (`{role:warrior=knight|mage=wizard|else=hero}`),
                        // otherwise it's positional (plurals / gender).
                        // Labelled mode is more verbose but generalises any
                        // enum-typed dispatch — gender just happens to be
                        // the most common case authors write positionally.
                        const bool labelled = forms.find('=') != std::string_view::npos;
                        if (labelled)
                        {
                            const auto selected = ResolveSelectForm(forms, it->second);
                            if (selected)
                                out.append(*selected);
                            else
                                out.append(pattern, i, closing - i + 1);
                        }
                        else
                        {
                            i32 count = 0;
                            if (TryParseInt(it->second, count))
                            {
                                // Numeric value → plural selection.
                                const u32 idx = ResolvePluralIndex(rule, count);
                                out.append(SelectPluralForm(forms, idx));
                            }
                            else if (auto genderIdx = TryParseGender(it->second))
                            {
                                // String value naming a gender → gender selection
                                // against the same positional form list. Authors
                                // write `{gender:le|la|leur}` and the engine picks
                                // by index 0/1/2 for masculine/feminine/neuter.
                                out.append(SelectPluralForm(forms, *genderIdx));
                            }
                            else
                            {
                                // Unrecognised value → leave the literal token so
                                // the bug is visible during testing.
                                out.append(pattern, i, closing - i + 1);
                            }
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
