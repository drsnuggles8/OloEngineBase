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
        MonoCircularBuffer<f32, 1920 * 2> Channels;   // Interleaved stereo sample data (L,R,L,R,...)

        i64 TotalFrames = 0;                    // Total frames in the source to be set by the reader on the first read, used by Wave Player
        i64 StartPosition = 0;                  // Frame position in source to wrap around when reached the end of the source
        i64 ReadPosition = 0;                   // Frame position in source to read next time from (this is where this source is being read by a NodeProcessor)
        u64 WaveHandle = 0;                     // Source Wave Asset handle
        const char* WaveName = nullptr;         // Wave Asset name for debugging purposes

        // Callback wrapper that encapsulates function pointer with context
        struct RefillCallback final
        {
            using FuncPtr = bool(*)(WaveSource&, void*) noexcept;
            FuncPtr funcPtr = nullptr;
            void* context = nullptr;

            RefillCallback() = default;
            RefillCallback(FuncPtr ptr, void* ctx = nullptr) noexcept : funcPtr(ptr), context(ctx) {}
            
            // Assignment from std::function for compatibility 
            RefillCallback& operator=(const std::function<bool(WaveSource&)>& func) noexcept
            {
                static thread_local std::function<bool(WaveSource&)> storedFunc;
                storedFunc = func;
                funcPtr = [](WaveSource& source, void*) noexcept -> bool {
                    try { return storedFunc(source); }
                    catch (...) { return false; }
                };
                context = nullptr;
                return *this;
            }

            [[nodiscard]] bool operator()(WaveSource& source) const noexcept
            {
                return funcPtr ? funcPtr(source, context) : false;
            }

            explicit operator bool() const noexcept { return funcPtr != nullptr; }
        };

        RefillCallback onRefill;

        [[nodiscard]] inline bool Refill() noexcept
        { 
            return onRefill(*this);
        }

        inline void Clear() noexcept
        {
            Channels.Clear();

            TotalFrames = 0;
            StartPosition = 0;
            ReadPosition = 0;
            WaveHandle = 0;
            WaveName = nullptr;
        }
    };

} // namespace OloEngine::Audio