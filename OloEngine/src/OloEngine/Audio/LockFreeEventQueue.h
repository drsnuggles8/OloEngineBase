#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <choc/containers/choc_Value.h>
#include <atomic>
#include <array>
#include <cstring>

namespace OloEngine::Audio
{
    //==============================================================================
    /// Pre-allocated storage for choc::value::Value data
    /// This avoids heap allocations in the audio thread by storing value data inline
    struct PreAllocatedValue
    {
        // Storage for the value data - large enough for most common types
        // Floats, ints, small vectors, etc. will fit inline
        static constexpr sizet s_InlineStorageSize = 64;
        alignas(8) u8 m_Storage[s_InlineStorageSize];
        
        // Track what type of data we have
        choc::value::Type m_Type;
        
        // Track actual size used
        u32 m_DataSize = 0;
        
        PreAllocatedValue() 
            : m_Type(choc::value::Type::createVoid())
        {
            std::memset(m_Storage, 0, s_InlineStorageSize);
        }
        
        // Copy constructor - always copy full storage for performance
        // Using constant size allows compiler to optimize with SIMD/unrolling
        PreAllocatedValue(const PreAllocatedValue& other)
            : m_Type(other.m_Type)
            , m_DataSize(other.m_DataSize)
        {
            std::memcpy(m_Storage, other.m_Storage, s_InlineStorageSize);
        }
        
        // Copy assignment - always copy full storage for performance  
        // Using constant size allows compiler to optimize with SIMD/unrolling
        PreAllocatedValue& operator=(const PreAllocatedValue& other)
        {
            if (this != &other)
            {
                m_Type = other.m_Type;
                m_DataSize = other.m_DataSize;
                std::memcpy(m_Storage, other.m_Storage, s_InlineStorageSize);
            }
            return *this;
        }
        
        /// Copy data from a choc::value::ValueView into our pre-allocated storage
        /// This is the key function - it avoids allocation by copying into inline storage
        /// Returns true if successful, false if data is too large
        bool CopyFrom(const choc::value::ValueView& source) noexcept
        {
            m_Type = source.getType();
            m_DataSize = static_cast<u32>(m_Type.getValueDataSize());
            
            // Check if it fits in our inline storage
            if (m_DataSize > s_InlineStorageSize)
            {
                // Data too large - this is a limitation of our pre-allocated approach
                // In practice, most audio events are small (floats, ints, small structs)
                return false;
            }
            
            // Copy the raw data into our storage
            const void* sourceData = source.getRawData();
            if (sourceData && m_DataSize > 0)
            {
                std::memcpy(m_Storage, sourceData, m_DataSize);
            }
            
            return true;
        }
        
        /// Create a ValueView pointing to our storage
        /// This allows code to access the value without allocation
        choc::value::ValueView GetView() noexcept
        {
            if (m_Type.isVoid() || m_DataSize == 0)
                return choc::value::ValueView();
                
            // Create a view pointing to our inline storage
            return choc::value::ValueView(m_Type, static_cast<void*>(m_Storage), nullptr);
        }
        
        /// Create an owned Value (this may allocate, but only when consuming on main thread)
        choc::value::Value GetValue() noexcept
        {
            if (m_Type.isVoid() || m_DataSize == 0)
                return choc::value::Value();
                
            // Create view and copy to Value
            return choc::value::Value(GetView());
        }
        
        void Clear() noexcept
        {
            m_Type = choc::value::Type::createVoid();
            m_DataSize = 0;
        }
    };
    
    //==============================================================================
    /// Event structure with pre-allocated value storage
    struct AudioThreadEvent
    {
        u64 m_FrameIndex = 0;
        u32 m_EndpointID = 0;  // Use u32 instead of Identifier for lock-free compatibility
        PreAllocatedValue m_ValueData;
        
        AudioThreadEvent() = default;
        
        // Copy constructor
        AudioThreadEvent(const AudioThreadEvent& other)
            : m_FrameIndex(other.m_FrameIndex)
            , m_EndpointID(other.m_EndpointID)
            , m_ValueData(other.m_ValueData)
        {}
        
        // Copy assignment
        AudioThreadEvent& operator=(const AudioThreadEvent& other)
        {
            if (this != &other)
            {
                m_FrameIndex = other.m_FrameIndex;
                m_EndpointID = other.m_EndpointID;
                m_ValueData = other.m_ValueData;
            }
            return *this;
        }
    };
    
    //==============================================================================
    /// Message structure with pre-allocated string storage
    struct AudioThreadMessage
    {
        u64 m_FrameIndex = 0;
        
        // Pre-allocated storage for message text
        static constexpr sizet s_MaxMessageLength = 256;
        char m_Text[s_MaxMessageLength] = {};
        
        AudioThreadMessage() = default;
        
        // Copy constructor
        // Performance-critical: Use fixed-size memcpy instead of strlen+dynamic copy
        // This is called on every queue Push/Pop operation
        AudioThreadMessage(const AudioThreadMessage& other)
            : m_FrameIndex(other.m_FrameIndex)
        {
            // Copy entire buffer - simple, fast, cache-friendly
            // No need for strlen (O(n) scan) since buffer is fixed size
            std::memcpy(m_Text, other.m_Text, s_MaxMessageLength);
        }
        
        // Copy assignment
        AudioThreadMessage& operator=(const AudioThreadMessage& other)
        {
            if (this != &other)
            {
                m_FrameIndex = other.m_FrameIndex;
                // Copy entire buffer - simple, fast, cache-friendly
                std::memcpy(m_Text, other.m_Text, s_MaxMessageLength);
            }
            return *this;
        }
        
        void SetText(const char* text) noexcept
        {
            if (text)
            {
                sizet len = std::strlen(text);
                sizet copyLen = std::min(len, s_MaxMessageLength - 1);
                std::memcpy(m_Text, text, copyLen);
                m_Text[s_MaxMessageLength - 1] = '\0';
                m_Text[copyLen] = '\0';
            }
            else
            {
                m_Text[0] = '\0';
            }
        }
    };
    
    //==============================================================================
    /// Lock-free SPSC (Single Producer Single Consumer) queue with pre-allocated storage
    /// This is real-time safe - no allocations, no locks, no blocking
    /// 
    /// Usage:
    ///   - Audio thread (producer) calls Push()
    ///   - Main thread (consumer) calls Pop()
    ///   - Size must be power of 2 for efficiency
    template<typename T, sizet Capacity>
    class LockFreeEventQueue
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
        static_assert(Capacity > 0, "Capacity must be greater than 0");
        
    public:
        LockFreeEventQueue()
        {
            m_WriteIndex.store(0, std::memory_order_relaxed);
            m_ReadIndex.store(0, std::memory_order_relaxed);
        }
        
        /// Push an item onto the queue (called from audio thread)
        /// Returns true if successful, false if queue is full
        /// This is wait-free and allocation-free
        bool Push(const T& item) noexcept
        {
            const sizet writeIndex = m_WriteIndex.load(std::memory_order_relaxed);
            const sizet nextWriteIndex = (writeIndex + 1) & (Capacity - 1);
            
            // Check if queue is full
            // We leave one slot empty to distinguish full from empty
            if (nextWriteIndex == m_ReadIndex.load(std::memory_order_acquire))
            {
                return false; // Queue is full
            }
            
            // Copy item into the buffer
            m_Buffer[writeIndex] = item;
            
            // Publish the write (release semantics ensure the data write is visible)
            m_WriteIndex.store(nextWriteIndex, std::memory_order_release);
            
            return true;
        }
        
        /// Try to pop an item from the queue (called from main thread)
        /// Returns true if an item was popped, false if queue is empty
        bool Pop(T& outItem) noexcept
        {
            const sizet readIndex = m_ReadIndex.load(std::memory_order_relaxed);
            
            // Check if queue is empty
            if (readIndex == m_WriteIndex.load(std::memory_order_acquire))
            {
                return false; // Queue is empty
            }
            
            // Read item from buffer
            outItem = m_Buffer[readIndex];
            
            // Publish the read
            const sizet nextReadIndex = (readIndex + 1) & (Capacity - 1);
            m_ReadIndex.store(nextReadIndex, std::memory_order_relaxed);
            
            return true;
        }
        
        /// Check if the queue is empty (approximate - may be stale)
        bool IsEmpty() const noexcept
        {
            return m_ReadIndex.load(std::memory_order_relaxed) == 
                   m_WriteIndex.load(std::memory_order_relaxed);
        }
        
        /// Get approximate number of items in queue (may be stale)
        sizet GetApproximateSize() const noexcept
        {
            const sizet write = m_WriteIndex.load(std::memory_order_relaxed);
            const sizet read = m_ReadIndex.load(std::memory_order_relaxed);
            
            if (write >= read)
                return write - read;
            else
                return Capacity - (read - write);
        }
        
        /// Clear the queue (only safe when no concurrent access)
        void Clear() noexcept
        {
            m_ReadIndex.store(0, std::memory_order_relaxed);
            m_WriteIndex.store(0, std::memory_order_relaxed);
        }
        
    private:
        // Ring buffer storage - pre-allocated at construction
        std::array<T, Capacity> m_Buffer;
        
        // Cache line padding to prevent false sharing between producer and consumer
        alignas(64) std::atomic<sizet> m_WriteIndex;
        alignas(64) std::atomic<sizet> m_ReadIndex;
    };
    
    //==============================================================================
    /// Convenient type aliases for common use cases
    
    /// Event queue - for audio events with value data
    template<sizet Capacity = 256>
    using AudioEventQueue = LockFreeEventQueue<AudioThreadEvent, Capacity>;
    
    /// Message queue - for debug/log messages from audio thread
    template<sizet Capacity = 256>
    using AudioMessageQueue = LockFreeEventQueue<AudioThreadMessage, Capacity>;
    
} // namespace OloEngine::Audio
