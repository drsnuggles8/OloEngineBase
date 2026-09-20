#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Memory block for storing command data.
    //
    // INVARIANT: Offset <= Size at all times, and Data is aligned to
    // ThreadLocalCache::BLOCK_ALIGNMENT.  Both are what make rollover safe —
    // see ThreadLocalCache::Allocate.
    struct MemoryBlock
    {
        u8* Data = nullptr;
        sizet Size = 0;
        sizet Offset = 0;
        MemoryBlock* Next = nullptr;
    };

    class ThreadLocalCache
    {
      public:
        // Default alignment of an unqualified Allocate() call.
        static constexpr sizet DEFAULT_ALIGNMENT = 8;

        // Largest alignment Allocate() honours, and the alignment every block's
        // Data is allocated with.  Because a block base is BLOCK_ALIGNMENT-
        // aligned, a *fresh* block satisfies any supported request at offset 0
        // with zero padding — which is what lets rollover reserve exactly the
        // requested size instead of guessing padding for an address it does not
        // have yet.  Requests above this are rejected rather than silently
        // mis-aligned.
        static constexpr sizet MAX_ALIGNMENT = 256;
        static constexpr sizet BLOCK_ALIGNMENT = MAX_ALIGNMENT;

        ThreadLocalCache(sizet blockSize);
        ~ThreadLocalCache();

        // Disallow copying
        ThreadLocalCache(const ThreadLocalCache&) = delete;
        ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;

        // Move operations
        ThreadLocalCache(ThreadLocalCache&& other) noexcept;
        ThreadLocalCache& operator=(ThreadLocalCache&& other) noexcept;

        // Allocate memory for a command.
        //
        // Returns nullptr — without advancing any cursor and leaving the cache
        // fully usable — when size is 0, when alignment is not a power of two
        // or exceeds MAX_ALIGNMENT, when the size/offset arithmetic would
        // overflow, or when the backing allocation fails.
        void* Allocate(sizet size, sizet alignment = DEFAULT_ALIGNMENT);

        // Reset the allocator - doesn't free memory, just resets offsets
        void Reset();

        // Completely free all memory
        void FreeAll();

        // Add a new block to the chain (or rewind a reusable one).  Returns
        // false and leaves the cache untouched if the block cannot be created.
        bool AddBlock(sizet minSize);

        sizet GetTotalAllocated() const
        {
            return m_TotalAllocated;
        }

        sizet GetWastedMemory() const
        {
            return m_WastedMemory;
        }

        // The block Allocate() is currently bumping.  Exposed so callers and
        // tests can assert that a returned pointer lies inside it.
        const MemoryBlock* GetCurrentBlock() const
        {
            return m_CurrentBlock;
        }

        // Walk the block list and return how many blocks exist.
        u32 GetBlockCount() const
        {
            u32 count = 0;
            for (MemoryBlock* b = m_FirstBlock; b != nullptr; b = b->Next)
            {
                ++count;
            }
            return count;
        }

      private:
        MemoryBlock* m_CurrentBlock = nullptr;
        MemoryBlock* m_FirstBlock = nullptr;
        sizet m_DefaultBlockSize = 0;
        sizet m_TotalAllocated = 0;
        sizet m_WastedMemory = 0;
    };
} // namespace OloEngine
