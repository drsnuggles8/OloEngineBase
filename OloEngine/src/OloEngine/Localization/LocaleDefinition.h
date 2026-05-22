#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    enum class TextDirection : u8
    {
        LTR = 0,
        RTL = 1,
    };

    // Determines which inflection form is selected for `{count:form0|form1|...}`
    // tokens. Names follow CLDR plural categories. We map only the categories
    // we need today: every supported locale falls into either OneOther
    // (English / German / most Romance) or OtherOnly (Japanese / Chinese /
    // Korean — no number agreement). More elaborate rule sets (Polish, Russian,
    // Arabic) can be added without changing the LocalizationManager API.
    enum class PluralRule : u8
    {
        OneOther = 0,
        OtherOnly = 1,
    };

    struct LocaleDefinition
    {
        std::string Code;
        std::string Name;
        TextDirection Direction = TextDirection::LTR;
        PluralRule Plural = PluralRule::OneOther;
    };

    // Resolve a count to a plural-form index suitable for indexing into the
    // pipe-separated form list inside a `{count:a|b|c}` token. The OneOther
    // rule returns 0 for |count| == 1, else 1 — matching English / German.
    // OtherOnly always returns 0, so an `{n:apple|apples}` token will pick
    // "apple" in Japanese regardless of count (caller is expected to author
    // a single form for such locales).
    inline u32 ResolvePluralIndex(PluralRule rule, i32 count) noexcept
    {
        switch (rule)
        {
            case PluralRule::OneOther:
                return (count == 1 || count == -1) ? 0u : 1u;
            case PluralRule::OtherOnly:
                return 0u;
        }
        return 1u;
    }
} // namespace OloEngine
