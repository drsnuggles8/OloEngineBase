#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Memory block for storing command data
    struct MemoryBlock
    {
        u8* Data = nullptr;
        sizet Size = 0;
        sizet Offset = 0;
        MemoryBlock* Next = nullptr;
    };

    // Thread-local memory cache for command allocations
    class ThreadLocalCache
    {
    public:
        ThreadLocalCache(sizet blockSize);
        ~ThreadLocalCache();

        // Disallow copying
        ThreadLocalCache(const ThreadLocalCache&) = delete;
        ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;

        // Move operations
        ThreadLocalCache(ThreadLocalCache&& other) noexcept;
        ThreadLocalCache& operator=(ThreadLocalCache&& other) noexcept;

        // Allocate memory for a command
        void* Allocate(sizet size, sizet alignment = 8);
        
        // Reset the allocator - doesn't free memory, just resets offsets
        void Reset();
        
        // Completely free all memory
        void FreeAll();
        
        // Add a new block to the chain
        void AddBlock(sizet minSize);

        sizet GetTotalAllocated() const { return m_TotalAllocated; }

    private:
        MemoryBlock* m_CurrentBlock = nullptr;
        MemoryBlock* m_FirstBlock = nullptr;
        sizet m_DefaultBlockSize = 0;
        sizet m_TotalAllocated = 0;
        sizet m_WastedMemory = 0;
    };
}
