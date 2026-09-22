#pragma once

#include <string>

// Shared implementation helpers for the annotation and component-field parsers.
namespace OloHeaderTool::Detail
{
    inline std::string Trim(const std::string& s)
    {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    inline std::string StripPrefix(const std::string& name, const std::string& prefix)
    {
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix)
            return name.substr(prefix.size());
        return name;
    }

    // If `s` begins with an `OLO_SERIALIZE(...)` marker (optionally whitespace before
    // the '('), peel it: `args` receives the text inside the outer parens (no parens)
    // and `rest` the leading-trimmed remainder after the closing ')', and returns
    // true. Otherwise returns false and leaves `args`/`rest` untouched. Shared by the
    // OLO_PROPERTY scanner (ParseHeaders — to peel a marker glued onto a field's
    // anchor line) and the scene-serializer field scan (ParseComponentFields — to
    // honour Skip), so both treat the glued and own-line forms identically. An
    // unterminated marker (no matching ')') yields an empty `rest` so the field is
    // safely skipped rather than mis-parsed; such source fails to compile anyway.
    inline bool PeelSerializeMarker(const std::string& s, std::string& args, std::string& rest)
    {
        static const std::string kMarker = "OLO_SERIALIZE";
        if (s.rfind(kMarker, 0) != 0)
            return false;
        size_t j = kMarker.size();
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
            ++j;
        if (j >= s.size() || s[j] != '(')
            return false; // a bare identifier starting with OLO_SERIALIZE, not a call

        int depth = 0;
        size_t closeParen = std::string::npos;
        for (size_t k = j; k < s.size(); ++k)
        {
            if (s[k] == '(')
                ++depth;
            else if (s[k] == ')' && --depth == 0)
            {
                closeParen = k;
                break;
            }
        }
        if (closeParen == std::string::npos)
        {
            args = s.substr(j + 1);
            rest.clear();
            return true;
        }
        args = s.substr(j + 1, closeParen - (j + 1));
        rest = Trim(s.substr(closeParen + 1));
        return true;
    }

    inline std::string ReplaceAll(std::string s, const std::string& from, const std::string& to)
    {
        for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
            s.replace(pos, from.size(), to);
        return s;
    }

} // namespace OloHeaderTool::Detail
