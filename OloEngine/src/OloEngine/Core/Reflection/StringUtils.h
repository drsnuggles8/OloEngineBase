#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>
#include <array>

namespace OloEngine::Core::Reflection::StringUtils {

	//==============================================================================
	/// Constexpr string operations

	constexpr bool StartsWith(std::string_view text, std::string_view prefix)
	{
		auto len = prefix.length();
		return text.length() >= len && text.substr(0, len) == prefix;
	}

	constexpr bool EndsWith(std::string_view text, std::string_view suffix)
	{
		auto len1 = text.length(), len2 = suffix.length();
		return len1 >= len2 && text.substr(len1 - len2) == suffix;
	}

	constexpr sizet CountTokens(std::string_view source, std::string_view delimiter)
	{
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

		if (N == 0) return tokens;

		// Guard against empty delimiter - return source as single token
		if (delimiter.empty())
		{
			if (N > 0)
				tokens[0] = source;
			return tokens;
		}

		sizet tokenStart = 0;
		sizet pos = 0;
		sizet i = 0;

		while (pos < source.length() && i < N - 1)
		{
			// Check if we have enough characters left for the delimiter
			if (pos + delimiter.length() <= source.length() && 
				source.substr(pos, delimiter.length()) == delimiter)
			{
				tokens[i] = source.substr(tokenStart, pos - tokenStart);
				pos += delimiter.length();
				tokenStart = pos;
				++i;
			}
			else
			{
				++pos;
			}
		}

		// Last token gets everything remaining
		if (i < N && tokenStart < source.length())
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
			name.remove_prefix(3);  // sizeof("in_") - 1
		else if (StartsWith(name, "out_"))
			name.remove_prefix(4);  // sizeof("out_") - 1
		else if (StartsWith(name, "m_"))
			name.remove_prefix(2);  // sizeof("m_") - 1

		// Remove common suffixes
		if (EndsWith(name, "_Raw"))
			name.remove_suffix(4);  // sizeof("_Raw") - 1

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