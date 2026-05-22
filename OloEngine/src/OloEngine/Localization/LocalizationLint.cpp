#include "OloEnginePCH.h"
#include "OloEngine/Localization/LocalizationLint.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Core/UTF8.h"

namespace OloEngine
{
    std::unordered_set<std::string> LocalizationLint::ExtractParameters(const std::string& value)
    {
        // Mirror TextFormatter's tokenisation rules: `{{` / `}}` escape, an
        // unmatched `{` ends extraction, and `{name:...}` plural tokens
        // contribute the bare parameter name.
        std::unordered_set<std::string> out;
        const sizet n = value.size();
        for (sizet i = 0; i < n; ++i)
        {
            const char c = value[i];
            if (c == '{')
            {
                if (i + 1 < n && value[i + 1] == '{')
                {
                    ++i;
                    continue;
                }
                const sizet closing = value.find('}', i + 1);
                if (closing == std::string::npos)
                    break;
                std::string token = value.substr(i + 1, closing - i - 1);
                const auto colon = token.find(':');
                if (colon != std::string::npos)
                    token.resize(colon);
                if (!token.empty())
                    out.insert(std::move(token));
                i = closing;
            }
        }
        return out;
    }

    std::vector<LocalizationLint::Issue> LocalizationLint::RunParameterDriftLint(
        const std::string& sourceLocaleCode,
        const std::vector<std::string>& targetLocaleCodes)
    {
        std::vector<Issue> issues;
        const auto loaded = LocalizationManager::GetAvailableLocales();
        if (loaded.empty())
            return issues;

        const std::string source = sourceLocaleCode.empty() ? loaded.front().Code : sourceLocaleCode;

        std::vector<std::string> targets = targetLocaleCodes;
        if (targets.empty())
        {
            targets.reserve(loaded.size());
            for (const auto& loc : loaded)
                if (loc.Code != source)
                    targets.push_back(loc.Code);
        }

        const auto sourceKeys = LocalizationManager::GetAllKeys(source);
        for (const auto& key : sourceKeys)
        {
            const auto sourceParams = ExtractParameters(LocalizationManager::Get(key, source));
            for (const auto& targetCode : targets)
            {
                if (!LocalizationManager::HasKey(key, targetCode))
                    continue; // missing-key reporting handles this case separately

                const auto targetParams = ExtractParameters(LocalizationManager::Get(key, targetCode));

                Issue issue;
                for (const auto& p : sourceParams)
                    if (!targetParams.contains(p))
                        issue.MissingTokens.insert(p);
                for (const auto& p : targetParams)
                    if (!sourceParams.contains(p))
                        issue.ExtraTokens.insert(p);

                if (issue.MissingTokens.empty() && issue.ExtraTokens.empty())
                    continue;

                issue.Key = key;
                issue.SourceLocale = source;
                issue.TargetLocale = targetCode;

                std::string desc = "parameter drift: ";
                if (!issue.MissingTokens.empty())
                {
                    desc += "missing {";
                    bool first = true;
                    for (const auto& t : issue.MissingTokens)
                    {
                        if (!first)
                            desc += ", ";
                        desc += t;
                        first = false;
                    }
                    desc += "}";
                }
                if (!issue.ExtraTokens.empty())
                {
                    if (!issue.MissingTokens.empty())
                        desc += "; ";
                    desc += "extra {";
                    bool first = true;
                    for (const auto& t : issue.ExtraTokens)
                    {
                        if (!first)
                            desc += ", ";
                        desc += t;
                        first = false;
                    }
                    desc += "}";
                }
                issue.Description = std::move(desc);
                issues.push_back(std::move(issue));
            }
        }
        return issues;
    }

    std::vector<LocalizationLint::Issue> LocalizationLint::RunMaxLengthLint(
        const std::string& sourceLocaleCode,
        const std::vector<std::string>& targetLocaleCodes)
    {
        std::vector<Issue> issues;
        const auto loaded = LocalizationManager::GetAvailableLocales();
        if (loaded.empty())
            return issues;

        const std::string source = sourceLocaleCode.empty() ? loaded.front().Code : sourceLocaleCode;

        std::vector<std::string> targets = targetLocaleCodes;
        if (targets.empty())
        {
            targets.reserve(loaded.size());
            for (const auto& loc : loaded)
                targets.push_back(loc.Code); // include source — translators may overflow the budget in the source language too
        }

        const auto sourceKeys = LocalizationManager::GetAllKeys(source);
        for (const auto& key : sourceKeys)
        {
            const auto md = LocalizationManager::GetMetadata(key, source);
            if (md.MaxLength == 0u)
                continue; // no budget declared
            for (const auto& targetCode : targets)
            {
                if (!LocalizationManager::HasKey(key, targetCode))
                    continue;
                const std::string value = LocalizationManager::Get(key, targetCode);
                const sizet codepoints = UTF8::CountCodepoints(value);
                if (codepoints <= md.MaxLength)
                    continue;
                Issue issue;
                issue.Key = key;
                issue.SourceLocale = source;
                issue.TargetLocale = targetCode;
                issue.Description = "max_length exceeded: " + std::to_string(codepoints) +
                                    " codepoints > budget " + std::to_string(md.MaxLength);
                issues.push_back(std::move(issue));
            }
        }
        return issues;
    }
} // namespace OloEngine
