#pragma once

// Denormal float handling for real-time audio DSP.
// Denormalized floats (subnormals) cause severe CPU stalls on x86.
// This utility flushes them to zero to keep DSP processing fast.
//
// Original concept by Jezar at Dreampoint (public domain).

#include <cstdint>
#include <cstring>

namespace OloEngine::Audio::DSP
{
    inline void Undenormalize(float& sample)
    {
        uint32_t bits;
        std::memcpy(&bits, &sample, sizeof(bits));
        if ((bits & 0x7F800000) == 0)
        {
            sample = 0.0f;
        }
    }
} // namespace OloEngine::Audio::DSP
