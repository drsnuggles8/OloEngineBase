#pragma once

#include "OloEngine/Core/Base.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace OloEngine
{
    enum class TextDirection : u8
    {
        LTR = 0,
        RTL = 1,
    };

    // Determines which inflection form is selected for `{count:form0|form1|...}`
    // tokens. Indices into the form list follow CLDR plural categories in
    // their canonical order: zero, one, two, few, many, other — but each rule
    // names only the forms it actually uses, and the index in the pipe-
    // separated form list matches that compact order.
    //
    //   OneOther:    [one, other]                        — en, de, it, es, pt
    //   OtherOnly:   [other]                              — ja, zh, ko, vi, th
    //   FrenchLike:  [one, other]                         — fr (0 and 1 → "one")
    //   PolishLike:  [one, few, many]                     — pl, cs, sk
    //   RussianLike: [one, few, many, other]              — ru, uk, sr, hr
    //   ArabicLike:  [zero, one, two, few, many, other]   — ar
    //
    // Rule formulas mirror Unicode CLDR (cldr-common/common/supplemental/
    // plurals.xml); negative counts are handled by taking |count| before
    // applying the rule, since CLDR is defined over non-negative integers.
    enum class PluralRule : u8
    {
        OneOther = 0,
        OtherOnly = 1,
        FrenchLike = 2,
        PolishLike = 3,
        RussianLike = 4,
        ArabicLike = 5,
    };

    struct LocaleDefinition
    {
        std::string Code;
        std::string Name;
        TextDirection Direction = TextDirection::LTR;
        PluralRule Plural = PluralRule::OneOther;
        // Number-formatting hints. Defaults match en-US conventions; locale
        // YAML overrides via `thousand_separator: "."` / `decimal_separator: ","`.
        // Empty thousand_separator means no grouping is applied.
        std::string ThousandSeparator = ",";
        std::string DecimalSeparator = ".";

        // Optional path to the primary font this locale should render with,
        // relative to the asset root. When set, LocalizationSystem assigns
        // it to every entity's TextComponent.FontAsset alongside the
        // LocalizationKey resolve. Leave empty to keep whichever font the
        // entity already has (the editor-authored default).
        std::string FontPath;

        // Optional fallback-font paths walked when the primary font lacks
        // a glyph. A typical setup is "primary Latin + CJK fallback +
        // emoji fallback". Same path resolution as FontPath.
        std::vector<std::string> FontFallbacks;
    };

    // Resolve a count to a plural-form index suitable for indexing into the
    // pipe-separated form list inside a `{count:a|b|c}` token. The index
    // semantics per rule are spelled out alongside PluralRule above.
    inline u32 ResolvePluralIndex(PluralRule rule, i32 count) noexcept
    {
        const u32 n = static_cast<u32>(std::abs(count));
        const u32 mod10 = n % 10u;
        const u32 mod100 = n % 100u;

        switch (rule)
        {
            case PluralRule::OneOther:
                // en/de/it/es/pt: 1 → one, else → other.
                return (n == 1u) ? 0u : 1u;

            case PluralRule::OtherOnly:
                // ja/zh/ko/etc: no number agreement.
                return 0u;

            case PluralRule::FrenchLike:
                // fr: 0 and 1 → one, else → other.
                return (n <= 1u) ? 0u : 1u;

            case PluralRule::PolishLike:
                // pl/cs/sk: one = {1}; few = {2..4, but not when the tens
                // digit is 1 (12..14)}; many = everything else.
                if (n == 1u)
                    return 0u;
                if (mod10 >= 2u && mod10 <= 4u && !(mod100 >= 12u && mod100 <= 14u))
                    return 1u;
                return 2u;

            case PluralRule::RussianLike:
                // ru/uk/sr/hr: one = mod10==1 && mod100!=11; few = mod10
                // in 2..4 && mod100 not in 12..14; many = mod10 in {0, 5..9}
                // OR mod100 in 11..14; other = the rare leftover (floats etc;
                // since we only handle integers, this branch is unused but we
                // keep it as the explicit fallback per CLDR).
                if (mod10 == 1u && mod100 != 11u)
                    return 0u;
                if (mod10 >= 2u && mod10 <= 4u && !(mod100 >= 12u && mod100 <= 14u))
                    return 1u;
                if (mod10 == 0u || (mod10 >= 5u && mod10 <= 9u) || (mod100 >= 11u && mod100 <= 14u))
                    return 2u;
                return 3u;

            case PluralRule::ArabicLike:
                // ar: zero={0}, one={1}, two={2}, few=mod100 in 3..10,
                // many=mod100 in 11..99, other=everything else (e.g. 100..102).
                if (n == 0u)
                    return 0u;
                if (n == 1u)
                    return 1u;
                if (n == 2u)
                    return 2u;
                if (mod100 >= 3u && mod100 <= 10u)
                    return 3u;
                if (mod100 >= 11u && mod100 <= 99u)
                    return 4u;
                return 5u;
        }
        return 0u;
    }
} // namespace OloEngine
