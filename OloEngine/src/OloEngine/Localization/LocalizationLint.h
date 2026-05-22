#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    // Static-analysis pass over loaded locales. Currently:
    //   - Detects parameter drift: translations that drop, add, or rename
    //     `{name}` substitution tokens compared to the source locale. A
    //     missing parameter shows up at runtime as a literal `{name}` in
    //     the rendered string; an extra one silently does nothing. Both
    //     are caught here pre-shipping.
    //
    // The lint pass walks every key that exists in the source locale.
    // Per-locale `{name}` token sets are extracted with simple
    // brace-matching (the same lexer logic TextFormatter uses).
    // `{name:foo|bar}` plural-form parameters count as `{name}`.
    class LocalizationLint
    {
      public:
        struct Issue
        {
            std::string Key;
            std::string SourceLocale;                      // e.g. "en"
            std::string TargetLocale;                      // e.g. "de"
            std::string Description;                       // human-readable summary for the editor
            std::unordered_set<std::string> MissingTokens; // present in source, absent in target
            std::unordered_set<std::string> ExtraTokens;   // present in target, absent in source
        };

        // `sourceLocaleCode` defaults to the alphabetically first loaded
        // locale (matching the LocalizationPanel's default reference choice).
        // If `targetLocaleCodes` is empty, every loaded locale != source is
        // checked.
        [[nodiscard("Store this!")]] static std::vector<Issue> RunParameterDriftLint(
            const std::string& sourceLocaleCode = {},
            const std::vector<std::string>& targetLocaleCodes = {});

        // Check every loaded translation against the per-key `max_length`
        // metadata declared on the source locale (typically authored to
        // match a fixed-width UI widget). A translation that exceeds the
        // budget — measured in Unicode codepoints, not bytes — produces an
        // issue. Translations without a max_length on the source are
        // ignored.
        [[nodiscard("Store this!")]] static std::vector<Issue> RunMaxLengthLint(
            const std::string& sourceLocaleCode = {},
            const std::vector<std::string>& targetLocaleCodes = {});

        // Extract the set of `{name}` substitution tokens from a single
        // string. Plural forms (`{count:singular|plural}`) yield the bare
        // parameter name (`count`). Exposed publicly so the editor or tests
        // can use the same canonical extraction.
        [[nodiscard("Store this!")]] static std::unordered_set<std::string> ExtractParameters(const std::string& value);
    };
} // namespace OloEngine
