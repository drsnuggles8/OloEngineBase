#include "OloEnginePCH.h"

#include "TestXmlOutputPath.h"

#include <cctype>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // gtest's own rule, matched exactly rather than approximated:
        // FilePath::IsDirectory() is "the last character is a path separator",
        // and that is precisely when GenerateUniqueFileName() — the racing
        // function — is reached. Both separators are accepted on every platform;
        // a trailing backslash on POSIX is pathological either way, and treating
        // it as a directory is the safe reading.
        [[nodiscard]] bool EndsWithSeparator(std::string_view path)
        {
            return !path.empty() && (path.back() == '/' || path.back() == '\\');
        }
    } // namespace

    std::string SanitizeForFilename(std::string_view text, std::string::size_type maxLength)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '_' || c == '-';
            if (safe)
            {
                out.push_back(c);
            }
            else if (!out.empty() && out.back() != '_')
            {
                // Collapse a run of unsafe characters to a single '_' so
                // `Suite.*-A:B:C` does not become a wall of underscores.
                out.push_back('_');
            }
        }
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        if (out.size() > maxLength)
            out.resize(maxLength);
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        return out;
    }

    std::string_view PositiveFilter(std::string_view filter)
    {
        const auto dash = filter.find('-');
        return (dash == std::string_view::npos) ? filter : filter.substr(0, dash);
    }

    std::string UniqueGTestOutputSpec(std::string_view outputFlag, std::string_view filter,
                                      std::string_view shardIndex, int pid)
    {
        const auto colon = outputFlag.find(':');
        if (colon == std::string_view::npos)
            return {}; // "xml" alone, or empty — gtest's own default name, not the directory path.

        const std::string_view format = outputFlag.substr(0, colon);
        const std::string_view directory = outputFlag.substr(colon + 1);

        // Only the two formats gtest actually writes. Anything else it warns
        // about and ignores, and rewriting it would hide that warning.
        if (format != "xml" && format != "json")
            return {};
        if (!EndsWithSeparator(directory))
            return {}; // An explicit file: already unique, leave it exactly as given.

        std::string token = SanitizeForFilename(PositiveFilter(filter));
        if (token.empty())
            token = "all"; // No filter at all — the whole-suite run.
        if (!shardIndex.empty())
        {
            token += "_shard";
            token += SanitizeForFilename(shardIndex, 16);
        }

        std::string spec;
        spec += format;
        spec += ':';
        spec += directory;
        spec += "OloEngine-Tests_";
        spec += token;
        spec += '_';
        spec += std::to_string(pid);
        spec += '.';
        spec += format;
        return spec;
    }
} // namespace OloEngine::Tests
