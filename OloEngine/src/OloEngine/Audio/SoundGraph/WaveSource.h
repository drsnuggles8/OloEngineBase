#pragma once

#include "OloEngine/Audio/Buffer/CircularBuffer.h"
#include <functional>
#include <atomic>

namespace OloEngine::Audio
{
    // Forward declaration
    struct AudioData;

    //==============================================================================
    /** Request from readers for new data when close to empty.
        Or check at the end/beginning of the audio callback
        from the wrapper.
    */
    struct WaveSource
    {
        MonoCircularBuffer<f32, 1920 * 2> m_Channels;   // Interleaved stereo sample data (L,R,L,R,...)

        i64 m_TotalFrames = 0;                    // Total frames in the source to be set by the reader on the first read, used by Wave Player
        i64 m_StartPosition = 0;                  // Frame position in source to wrap around when reached the end of the source
        i64 m_ReadPosition = 0;                   // Frame position in source to read next time from (this is where this source is being read by a NodeProcessor)
        u64 m_WaveHandle = 0;                     // Source Wave Asset handle
        const char* m_WaveName = nullptr;         // Wave Asset name for debugging purposes
        
        // Cached audio data pointer for lock-free access in audio thread
        // This should be set during initialization and remain valid for the lifetime of the WaveSource
        // Using atomic pointer ensures thread-safe access without locks in the realtime callback
        std::atomic<const AudioData*> m_CachedAudioData{nullptr};

        // Callback wrapper that encapsulates function pointer with context
        struct RefillCallback final
        {
            using FuncPtr = bool(*)(WaveSource&, void*) noexcept;

        public:
            RefillCallback() = default;
            RefillCallback(FuncPtr ptr, void* ctx = nullptr) noexcept : m_FuncPtr(ptr), m_Context(ctx) {}
            
            // Copy constructor - rebind to the new instance
            RefillCallback(const RefillCallback& other) 
                : m_InstanceFunc(other.m_InstanceFunc)
            {
                if (other.m_InstanceFunc)
                {
                    m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                        auto* self = static_cast<RefillCallback*>(ctx);
                        try { return self->m_InstanceFunc(source); }
                        catch (...) { return false; }
                    };
                    m_Context = this;
                }
                else
                {
                    m_FuncPtr = other.m_FuncPtr;
                    m_Context = other.m_Context;
                }
            }
            
            // Move constructor - rebind to the new instance
            RefillCallback(RefillCallback&& other) noexcept 
                : m_InstanceFunc(std::move(other.m_InstanceFunc))
            {
                if (m_InstanceFunc)
                {
                    m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                        auto* self = static_cast<RefillCallback*>(ctx);
                        try { return self->m_InstanceFunc(source); }
                        catch (...) { return false; }
                    };
                    m_Context = this;
                    other.m_FuncPtr = nullptr;
                    other.m_Context = nullptr;
                }
                else
                {
                    m_FuncPtr = other.m_FuncPtr;
                    m_Context = other.m_Context;
                    other.m_FuncPtr = nullptr;
                    other.m_Context = nullptr;
                }
            }
            
            // Copy assignment - rebind to this instance
            RefillCallback& operator=(const RefillCallback& other)
            {
                if (this != &other)
                {
                    m_InstanceFunc = other.m_InstanceFunc;
                    if (m_InstanceFunc)
                    {
                        m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                            auto* self = static_cast<RefillCallback*>(ctx);
                            try { return self->m_InstanceFunc(source); }
                            catch (...) { return false; }
                        };
                        m_Context = this;
                    }
                    else
                    {
                        m_FuncPtr = other.m_FuncPtr;
                        m_Context = other.m_Context;
                    }
                }
                return *this;
            }
            
            // Move assignment - rebind to this instance
            RefillCallback& operator=(RefillCallback&& other) noexcept
            {
                if (this != &other)
                {
                    m_InstanceFunc = std::move(other.m_InstanceFunc);
                    if (m_InstanceFunc)
                    {
                        m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                            auto* self = static_cast<RefillCallback*>(ctx);
                            try { return self->m_InstanceFunc(source); }
                            catch (...) { return false; }
                        };
                        m_Context = this;
                        other.m_FuncPtr = nullptr;
                        other.m_Context = nullptr;
                    }
                    else
                    {
                        m_FuncPtr = other.m_FuncPtr;
                        m_Context = other.m_Context;
                        other.m_FuncPtr = nullptr;
                        other.m_Context = nullptr;
                    }
                }
                return *this;
            }
            
            // Assignment from std::function - uses per-instance storage
            RefillCallback& operator=(const std::function<bool(WaveSource&)>& func)
            {
                m_InstanceFunc = func;
                m_FuncPtr = [](WaveSource& source, void* ctx) noexcept -> bool {
                    auto* self = static_cast<RefillCallback*>(ctx);
                    try { return self->m_InstanceFunc(source); }
                    catch (...) { return false; }
                };
                m_Context = this;
                return *this;
            }

            [[nodiscard]] bool operator()(WaveSource& source) const noexcept
            {
                return m_FuncPtr ? m_FuncPtr(source, m_Context) : false;
            }

            explicit operator bool() const noexcept { return m_FuncPtr != nullptr; }

            // Read-only accessors for inspection
            [[nodiscard]] FuncPtr GetFunctionPointer() const noexcept { return m_FuncPtr; }
            [[nodiscard]] void* GetContext() const noexcept { return m_Context; }
            [[nodiscard]] bool IsInstanceBacked() const noexcept { return static_cast<bool>(m_InstanceFunc); }

        private:
            FuncPtr m_FuncPtr = nullptr;
            void* m_Context = nullptr;
            
            // Per-instance storage to avoid cross-instance interference
            std::function<bool(WaveSource&)> m_InstanceFunc;
        };

        RefillCallback m_OnRefill;

        [[nodiscard]] inline bool Refill() noexcept
        { 
            OLO_PROFILE_FUNCTION();
            
            return m_OnRefill(*this);
        }

        inline void Clear() noexcept
        {
            m_Channels.Clear();

            m_TotalFrames = 0;
            m_StartPosition = 0;
            m_ReadPosition = 0;
            m_WaveHandle = 0;
            m_WaveName = nullptr;
        }
    };

} // namespace OloEngine::Audio