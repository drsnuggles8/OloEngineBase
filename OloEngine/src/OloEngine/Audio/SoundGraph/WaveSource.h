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
            FuncPtr m_FuncPtr = nullptr;
            void* context = nullptr;
            
            // Per-instance storage to avoid cross-instance interference
            std::function<bool(WaveSource&)> instanceFunc;

            RefillCallback() = default;
            RefillCallback(FuncPtr ptr, void* ctx = nullptr) noexcept : m_FuncPtr(ptr), context(ctx) {}
            
            // Copy constructor - rebind to the new instance
            RefillCallback(const RefillCallback& other) noexcept 
                : instanceFunc(other.instanceFunc)
            {
                if (other.instanceFunc)
                {
                    m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                        auto* self = static_cast<RefillCallback*>(ctx);
                        try { return self->instanceFunc(source); }
                        catch (...) { return false; }
                    };
                    context = this;
                }
                else
                {
                    m_FuncPtr = other.m_FuncPtr;
                    context = other.context;
                }
            }
            
            // Move constructor - rebind to the new instance
            RefillCallback(RefillCallback&& other) noexcept 
                : instanceFunc(std::move(other.instanceFunc))
            {
                if (instanceFunc)
                {
                    m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                        auto* self = static_cast<RefillCallback*>(ctx);
                        try { return self->instanceFunc(source); }
                        catch (...) { return false; }
                    };
                    context = this;
                    other.m_FuncPtr = nullptr;
                    other.context = nullptr;
                }
                else
                {
                    m_FuncPtr = other.m_FuncPtr;
                    context = other.context;
                    other.m_FuncPtr = nullptr;
                    other.context = nullptr;
                }
            }
            
            // Copy assignment - rebind to this instance
            RefillCallback& operator=(const RefillCallback& other) noexcept
            {
                if (this != &other)
                {
                    instanceFunc = other.instanceFunc;
                    if (instanceFunc)
                    {
                        m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                            auto* self = static_cast<RefillCallback*>(ctx);
                            try { return self->instanceFunc(source); }
                            catch (...) { return false; }
                        };
                        context = this;
                    }
                    else
                    {
                        m_FuncPtr = other.m_FuncPtr;
                        context = other.context;
                    }
                }
                return *this;
            }
            
            // Move assignment - rebind to this instance
            RefillCallback& operator=(RefillCallback&& other) noexcept
            {
                if (this != &other)
                {
                    instanceFunc = std::move(other.instanceFunc);
                    if (instanceFunc)
                    {
                        m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                            auto* self = static_cast<RefillCallback*>(ctx);
                            try { return self->instanceFunc(source); }
                            catch (...) { return false; }
                        };
                        context = this;
                        other.m_FuncPtr = nullptr;
                        other.context = nullptr;
                    }
                    else
                    {
                        m_FuncPtr = other.m_FuncPtr;
                        context = other.context;
                        other.m_FuncPtr = nullptr;
                        other.context = nullptr;
                    }
                }
                return *this;
            }
            
            // Assignment from std::function - uses per-instance storage
            RefillCallback& operator=(const std::function<bool(WaveSource&)>& func) noexcept
            {
                instanceFunc = func;
                m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                    auto* self = static_cast<RefillCallback*>(ctx);
                    try { return self->instanceFunc(source); }
                    catch (...) { return false; }
                };
                context = this;
                return *this;
            }

            [[nodiscard]] bool operator()(WaveSource& source) const noexcept
            {
                return m_FuncPtr ? m_FuncPtr(source, context) : false;
            }

            explicit operator bool() const noexcept { return m_FuncPtr != nullptr; }
        };

        RefillCallback m_OnRefill;

        [[nodiscard]] inline bool Refill() noexcept
        { 
            return m_OnRefill(*this);
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