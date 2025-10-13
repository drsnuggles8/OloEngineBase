#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"

#include <atomic>
#include <memory>

namespace OloEngine
{
    /**
     * @brief Lock-free fixed-size block allocator
     * 
     * This allocator manages a pool of fixed-size memory blocks using a lock-free
     * free list. It's designed for high-performance allocation/deallocation of
     * objects of uniform size, such as task objects or queue nodes.
     * 
     * Key features:
     * - Lock-free allocation and deallocation
     * - Fixed block size (specified at construction)
     * - Pre-allocated pool with optional growth
     * - Intrusive free list (uses the freed memory to store the next pointer)
     * - Thread-safe for concurrent access
     * 
     * Implementation uses a simple lock-free stack (Treiber stack) for the free list.
     * 
     * Thread Safety: Safe for concurrent access from multiple threads
     */
    class LockFreeAllocator
    {
    public:
        /**
         * @brief Construct allocator with specified block size and capacity
         * 
         * @param blockSize Size of each block in bytes (must be >= sizeof(void*))
         * @param initialCapacity Number of blocks to pre-allocate
         * @param alignment Memory alignment for blocks (default 16 bytes)
         */
        explicit LockFreeAllocator(sizet blockSize, sizet initialCapacity = 1024, sizet alignment = 16);
        
        /**
         * @brief Destructor - frees all allocated memory
         */
        ~LockFreeAllocator();

        // Disallow copying
        LockFreeAllocator(const LockFreeAllocator&) = delete;
        LockFreeAllocator& operator=(const LockFreeAllocator&) = delete;

        // Allow moving
        LockFreeAllocator(LockFreeAllocator&& other) noexcept;
        LockFreeAllocator& operator=(LockFreeAllocator&& other) noexcept;

        /**
         * @brief Allocate a block from the pool
         * 
         * This is a lock-free operation. If the free list is empty and growth
         * is disabled, returns nullptr.
         * 
         * @return Pointer to allocated block, or nullptr if allocation failed
         */
        void* Allocate();

        /**
         * @brief Return a block to the pool
         * 
         * This is a lock-free operation. The block must have been allocated
         * by this allocator.
         * 
         * @param ptr Pointer to block to free (must not be null)
         */
        void Free(void* ptr);

        /**
         * @brief Get the block size
         */
        sizet GetBlockSize() const { return m_BlockSize; }

        /**
         * @brief Get the alignment
         */
        sizet GetAlignment() const { return m_Alignment; }

        /**
         * @brief Get total capacity (number of blocks allocated)
         */
        sizet GetCapacity() const { return m_Capacity.load(std::memory_order_relaxed); }

        /**
         * @brief Get approximate number of free blocks
         * 
         * This is a relaxed check that may be inaccurate due to concurrent
         * modifications. Use only for statistics/debugging.
         */
        sizet GetApproximateFreeCount() const { return m_FreeCount.load(std::memory_order_relaxed); }

        /**
         * @brief Get approximate number of allocated blocks
         */
        sizet GetApproximateAllocatedCount() const 
        { 
            return m_Capacity.load(std::memory_order_relaxed) - m_FreeCount.load(std::memory_order_relaxed); 
        }

    private:
        /**
         * @brief Free list node - stored in freed memory blocks
         */
        struct FreeNode
        {
            FreeNode* Next;
        };

        /**
         * @brief Memory chunk - tracks a contiguous allocation from the system
         */
        struct MemoryChunk
        {
            void* Memory;           // Raw memory pointer
            sizet NumBlocks;        // Number of blocks in this chunk
            MemoryChunk* Next;      // Next chunk in chain
        };

        sizet m_BlockSize;          // Size of each block
        sizet m_Alignment;          // Alignment requirement
        
        alignas(128) std::atomic<FreeNode*> m_FreeList{nullptr};  // Lock-free free list
        alignas(128) std::atomic<sizet> m_Capacity{0};            // Total blocks allocated
        alignas(128) std::atomic<sizet> m_FreeCount{0};           // Approximate free count
        
        MemoryChunk* m_ChunkList{nullptr};  // List of memory chunks (accessed only during init/shutdown)

        /**
         * @brief Allocate a new chunk from the system and add blocks to free list
         * 
         * @param numBlocks Number of blocks to allocate
         * @return True if successful, false otherwise
         */
        bool AllocateChunk(sizet numBlocks);

        /**
         * @brief Free all memory chunks
         */
        void FreeAllChunks();
    };

} // namespace OloEngine
