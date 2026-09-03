#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace OloEngine::MCP
{
    [[nodiscard]] inline std::string NormalizeToken(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0)
                result.push_back(static_cast<char>(std::tolower(character)));
        }
        return result;
    }
} // namespace OloEngine::MCP
