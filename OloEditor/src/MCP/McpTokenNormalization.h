#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace OloEngine::MCP
{
    // Lowercase and drop separators, so "Ultra Performance", "ultra_performance"
    // and "ultra-performance" are one key. '+' is spelled "plus" rather than
    // dropped: dropping it turned "forward+" into "forward", which matched the
    // other render path with no error. Every MCP token table uses this one
    // function so the rules cannot drift between copies.
    [[nodiscard]] inline std::string NormalizeToken(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0)
                result.push_back(static_cast<char>(std::tolower(character)));
            else if (character == '+')
                result += "plus";
        }
        return result;
    }
} // namespace OloEngine::MCP
