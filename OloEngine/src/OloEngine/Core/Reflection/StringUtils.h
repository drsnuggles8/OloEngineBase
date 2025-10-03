#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>
#include <array>

namespace OloEngine::Core::Reflection::StringUtils {

    //==============================================================================
    /// Constexpr string operations

    constexpr bool StartsWith(std::string_view text, std::string_view prefix)
    {
        sizet len = prefix.length();
        return text.length() >= len && text.substr(0, len) == prefix;
    }

    constexpr bool EndsWith(std::string_view text, std::string_view suffix)
    {
        sizet len1 = text.length(), len2 = suffix.length();
        return len1 >= len2 && text.substr(len1 - len2) == suffix;
    }

    constexpr sizet CountTokens(std::string_view source, std::string_view delimiter)
    {
        if (source.empty())
        {
            return 0;
        }
        
        // Guard against empty delimiter to avoid infinite loops
        if (delimiter.empty())
            return source.empty() ? 0 : 1;
            
        sizet count = 1;
        sizet pos = 0;
        while ((pos = source.find(delimiter, pos)) != std::string_view::npos)
        {
            ++count;
            pos += delimiter.size();
        }
        return count;
    }

    template<sizet N>
    constexpr std::array<std::string_view, N> SplitString(std::string_view source, std::string_view delimiter)
    {
        std::array<std::string_view, N> tokens{};

        if constexpr (N == 0)
        {
            return tokens;
        }

        // Guard against empty delimiter - return source as single token
        if (delimiter.empty())
        {
            tokens[0] = source;
            return tokens;
        }

        sizet tokenStart = 0;
        sizet i = 0;

        while (i < N - 1)
        {
            sizet pos = source.find(delimiter, tokenStart);
            if (pos == std::string_view::npos)
                break;

            tokens[i] = source.substr(tokenStart, pos - tokenStart);
            tokenStart = pos + delimiter.size();
            ++i;
        }

        // Assign last token unconditionally (allows empty token if tokenStart == source.size())
        if (i < N)
            tokens[i] = source.substr(tokenStart);

        return tokens;
    }

    constexpr std::string_view RemoveNamespace(std::string_view name)
    {
        const auto pos = name.find_last_of(':');
        if (pos == std::string_view::npos)
            return name;
        
        return name.substr(pos + 1);
    }

    template<sizet N>
    constexpr std::array<std::string_view, N> RemoveNamespace(std::array<std::string_view, N> memberList)
    {
        for (std::string_view& fullName : memberList)
            fullName = RemoveNamespace(fullName);

        return memberList;
    }

    constexpr std::string_view RemovePrefixAndSuffix(std::string_view name)
    {
        // Remove common prefixes
        if (StartsWith(name, "in_"))
            name.remove_prefix(3);  // length of "in_"
        else if (StartsWith(name, "out_"))
            name.remove_prefix(4);  // length of "out_"
        else if (StartsWith(name, "m_"))
            name.remove_prefix(2);  // length of "m_"

        // Remove common suffixes
        if (EndsWith(name, "_Raw"))
            name.remove_suffix(4);  // length of "_Raw"

        return name;
    }

    template<sizet N>
    constexpr std::array<std::string_view, N> CleanMemberNames(std::array<std::string_view, N> memberList)
    {
        for (std::string_view& name : memberList)
            name = RemovePrefixAndSuffix(name);

        return memberList;
    }

    constexpr std::string_view ExtractNamespace(std::string_view fullName)
    {
        const auto pos = fullName.find_last_of(':');
        if (pos == std::string_view::npos)
            return {};
        
        return fullName.substr(0, pos == 0 ? 0 : pos - 1);  // Exclude the "::" itself
    }

    constexpr std::string_view ExtractClassName(std::string_view fullName)
    {
        const auto namespaceView = ExtractNamespace(fullName);
        const auto namespaceSize = namespaceView.size();
        
        if (namespaceSize == 0)
        {
            // Check for global-scope qualified name (starts with "::")
            if (fullName.size() >= 2 && fullName.rfind("::", 0) == 0)
            {
                return fullName.substr(2);  // Strip leading "::"
            }
            return fullName;
        }
        
        return fullName.substr(namespaceSize + 2);  // +2 for "::"
    }

} // namespace OloEngine::Core::Reflection::StringUtils