#pragma once

#include "OloEngine/Core/Base.h"

#include <array>

// Reads the block-mode field out of a packed BC6H block, independently of the encoder's
// own mode tables. Shared by BC6HModeCoverageTest (which checks a block carries the mode
// it was encoded for) and BC6HQualityTest (which reports which modes the automatic
// encoder actually picks per block class). Kept in one place so the two cannot disagree
// about what "mode 7" means.

namespace OloEngine::Tests
{
    // Two bits LSB-first, plus three more (shifted up) unless those two are 0b00 / 0b01.
    // Returns bcdec's internal mode index (0-13), or -1 for a reserved bit pattern.
    [[nodiscard]] inline i32 ReadBC6HModeIndex(const u8* block)
    {
        const auto bitAt = [block](u32 index) -> u32
        { return (block[index >> 3] >> (index & 7u)) & 1u; };

        u32 pattern = bitAt(0) | (bitAt(1) << 1);
        if (pattern <= 1)
            return static_cast<i32>(pattern); // patterns 0b00 / 0b01 are modes 0 and 1
        pattern |= (bitAt(2) | (bitAt(3) << 1) | (bitAt(4) << 2)) << 2;

        // bcdec's case labels, in its internal mode order (index 2 upwards).
        constexpr std::array<u32, 12> kPatterns = {
            0b00010, 0b00110, 0b01010, 0b01110, 0b10010, 0b10110,
            0b11010, 0b11110, 0b00011, 0b00111, 0b01011, 0b01111
        };
        for (u32 i = 0; i < static_cast<u32>(kPatterns.size()); ++i)
        {
            if (kPatterns[i] == pattern)
                return static_cast<i32>(i + 2);
        }
        return -1;
    }

    // True for the ten two-subset modes (0-9); modes 10-13 are one-subset.
    [[nodiscard]] inline bool IsBC6HTwoSubsetMode(i32 modeIndex)
    {
        return modeIndex >= 0 && modeIndex <= 9;
    }
} // namespace OloEngine::Tests
