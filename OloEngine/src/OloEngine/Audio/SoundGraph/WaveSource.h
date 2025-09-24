#pragma once

#include "OloEngine/Audio/Buffer/CircularBuffer.h"
#include <functional>

namespace OloEngine::Audio
{
    //==============================================================================
    /** Request from readers for new data when close to empty.
        Or check at the end/beginning of the audio callback
        from the wrapper.
    */
    struct WaveSource
    {
        CircularBuffer<f32, 1920> Channels;     // Interleaved sample data

        i64 TotalFrames = 0;                    // Total frames in the source to be set by the reader on the first read, used by Wave Player
        i64 StartPosition = 0;                  // Frame position in source to wrap around when reached the end of the source
        i64 ReadPosition = 0;                   // Frame position in source to read next time from (this is where this source is being read by a NodeProcessor)
        u64 WaveHandle = 0;                     // Source Wave Asset handle
        const char* WaveName = nullptr;         // Wave Asset name for debugging purposes

        using RefillCallback = std::function<bool(WaveSource&)>;
        RefillCallback onRefill = nullptr;

        inline bool Refill() { return onRefill(*this); }

        inline void Clear() noexcept
        {
            Channels.Clear();

            TotalFrames = 0;
            ReadPosition = 0;
            WaveHandle = 0;
            WaveName = nullptr;
        }
    };

} // namespace OloEngine::Audio