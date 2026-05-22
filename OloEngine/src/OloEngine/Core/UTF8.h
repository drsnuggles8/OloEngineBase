#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>

namespace OloEngine::UTF8
{
    // Decode a single UTF-8 codepoint starting at `bytes[start]`.
    //
    // On success: returns the Unicode scalar in `outCodepoint` and writes
    // the number of bytes consumed (1–4) to `outAdvance`.
    //
    // On invalid input (truncated, overlong, surrogate, out of range, etc.):
    // returns U+FFFD (REPLACEMENT CHARACTER) and advances by exactly one
    // byte so iteration always makes forward progress. Callers can detect
    // the error by comparing `outCodepoint` to U+FFFD if needed, but the
    // typical render-path use ignores invalid bytes.
    //
    // `start` must be < `bytes.size()` — the function does not check.
    inline void DecodeCodepoint(std::string_view bytes, sizet start, u32& outCodepoint, sizet& outAdvance) noexcept
    {
        const sizet remaining = bytes.size() - start;
        const auto b0 = static_cast<unsigned char>(bytes[start]);

        // ASCII fast-path
        if (b0 < 0x80u)
        {
            outCodepoint = b0;
            outAdvance = 1;
            return;
        }

        // Determine sequence length from the high bits of b0.
        u32 cp;
        sizet need; // total bytes including the lead byte
        if ((b0 & 0xE0u) == 0xC0u)
        {
            cp = b0 & 0x1Fu;
            need = 2;
        }
        else if ((b0 & 0xF0u) == 0xE0u)
        {
            cp = b0 & 0x0Fu;
            need = 3;
        }
        else if ((b0 & 0xF8u) == 0xF0u)
        {
            cp = b0 & 0x07u;
            need = 4;
        }
        else
        {
            // Invalid lead byte (continuation in lead position, or 5-/6-byte
            // sequence — those have been illegal since RFC 3629).
            outCodepoint = 0xFFFDu;
            outAdvance = 1;
            return;
        }

        if (remaining < need)
        {
            outCodepoint = 0xFFFDu;
            outAdvance = 1;
            return;
        }

        for (sizet i = 1; i < need; ++i)
        {
            const auto b = static_cast<unsigned char>(bytes[start + i]);
            if ((b & 0xC0u) != 0x80u)
            {
                outCodepoint = 0xFFFDu;
                outAdvance = 1;
                return;
            }
            cp = (cp << 6) | (b & 0x3Fu);
        }

        // Reject surrogates, codepoints above U+10FFFF, and overlong forms.
        const bool overlong =
            (need == 2 && cp < 0x80u) ||
            (need == 3 && cp < 0x800u) ||
            (need == 4 && cp < 0x10000u);
        const bool surrogate = (cp >= 0xD800u && cp <= 0xDFFFu);
        const bool tooLarge = (cp > 0x10FFFFu);
        if (overlong || surrogate || tooLarge)
        {
            outCodepoint = 0xFFFDu;
            outAdvance = 1;
            return;
        }

        outCodepoint = cp;
        outAdvance = need;
    }

    // Count how many UTF-8 codepoints are in `bytes`. Invalid sequences
    // are counted as one replacement codepoint each (matching the
    // decoder's "advance by 1 byte on error" recovery).
    inline sizet CountCodepoints(std::string_view bytes) noexcept
    {
        sizet count = 0;
        sizet i = 0;
        while (i < bytes.size())
        {
            u32 cp = 0;
            sizet adv = 0;
            DecodeCodepoint(bytes, i, cp, adv);
            i += adv;
            ++count;
        }
        return count;
    }
} // namespace OloEngine::UTF8
