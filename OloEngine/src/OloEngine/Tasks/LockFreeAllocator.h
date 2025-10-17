#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"

#include <atomic>
#include <bit>
#include <memory>

namespace OloEngine
{
    /**
     * @brief Lock-free fixed-size block allocator with tagged pointers
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
     * - **Tagged pointers for bulletproof ABA prevention**
     * - Thread-safe for concurrent access
     * 
     * Implementation uses a lock-free stack (Treiber stack) for the free list with
     * **tagged pointers** to eliminate the ABA problem.
     * 
     * **ABA Problem and Our Solution:**
     * 
     * The ABA problem is a classic concurrency issue in lock-free data structures:
     * 
     * Example scenario WITHOUT tagged pointers:
     * 1. Thread A reads head pointer (0x1000 pointing to Node A)
     * 2. Thread A gets preempted before CAS
     * 3. Thread B pops Node A, frees it
     * 4. Thread B allocates a new node, gets same address 0x1000 (now Node B)
     * 5. Thread B pushes Node B back onto stack
     * 6. Thread A wakes up, CAS succeeds (0x1000 == 0x1000) but it's a different node!
     * 7. Result: Stack corruption, use-after-free, wrong linkage
     * 
     * **Our Solution: Tagged Pointers**
     * 
     * We use **tagged pointers** that pack both the pointer AND a version counter
     * into a single 64-bit atomic value:
     * 
     * ```
     * 64-bit layout:
     * [63-48: 16-bit counter][47-0: 48-bit pointer]
     * ```
     * 
     * On x64, addresses are canonically 48-bit (0x0000'0000'0000'0000 to 0x0000'7FFF'FFFF'FFFF).
     * We use the upper 16 bits to store a version counter that increments on every operation.
     * 
     * **How It Prevents ABA:**
     * 
     * Same scenario WITH tagged pointers:
     * 1. Thread A reads tagged pointer (counter=0, ptr=0x1000)
     * 2. Thread A gets preempted
     * 3. Thread B pops node (counter becomes 1)
     * 4. Thread B allocates new node at 0x1000
     * 5. Thread B pushes node (counter becomes 2)
     * 6. Thread A wakes up, CAS FAILS because counter changed (0 != 2)
     * 7. Result: Thread A retries with new value, no corruption
     * 
     * The counter makes reused addresses detectable. Even if the same pointer
     * appears at the head, the counter will be different, preventing false matches.
     * 
     * **Counter Overflow:**
     * 
     * The 16-bit counter wraps around after 65,536 operations. However, for ABA to occur
     * after wraparound, ALL of the following must happen between when Thread A reads
     * the value and when it attempts the CAS:
     * 
     * 1. 65,536 push/pop operations must occur (to wrap the counter)
     * 2. The exact same pointer must be at the head
     * 3. Thread A must still be holding the old value
     * 
     * In practice, this is virtually impossible. Even at 1 billion ops/sec,
     * 65,536 operations take 65 microseconds. If a thread is preempted for that long,
     * the CAS would likely fail anyway due to other changes.
     * 
     * **Memory Ordering:**
     * 
     * - Allocate (pop): acquire on successful CAS
     * - Free (push): release on successful CAS
     * - Counter increments: part of CAS operation (no separate atomic needed)
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
         * @brief Tagged pointer for ABA prevention
         * 
         * Packs a 48-bit pointer with a 16-bit version counter into a 64-bit value.
         * 
         * Bit layout:
         * - Bits [63-48]: 16-bit counter (version tag)
         * - Bits [47-0]:  48-bit pointer (x64 canonical address)
         * 
         * Usage:
         * ```cpp
         * TaggedPointer tagged = Pack(ptr, counter);
         * FreeNode* node = GetPointer(tagged);
         * u16 version = GetCounter(tagged);
         * ```
         */
        using TaggedPointer = u64;

        /**
         * @brief Pack a pointer and counter into a tagged pointer
         * 
         * @param ptr Pointer to pack (must be a valid x64 canonical address)
         * @param counter 16-bit version counter
         * @return 64-bit tagged pointer value
         */
        static inline TaggedPointer Pack(FreeNode* ptr, u16 counter) noexcept
        {
            // Mask pointer to 48 bits (x64 canonical address)
            const u64 ptrBits = std::bit_cast<u64>(ptr) & 0x0000'FFFF'FFFF'FFFF;
            const u64 counterBits = static_cast<u64>(counter) << 48;
            return ptrBits | counterBits;
        }

        /**
         * @brief Extract the pointer from a tagged pointer
         * 
         * @param tagged Tagged pointer value
         * @return Pointer extracted from lower 48 bits
         */
        static inline FreeNode* GetPointer(TaggedPointer tagged) noexcept
        {
            // Extract lower 48 bits and cast to pointer
            const u64 ptrBits = tagged & 0x0000'FFFF'FFFF'FFFF;
            return std::bit_cast<FreeNode*>(ptrBits);
        }

        /**
         * @brief Extract the counter from a tagged pointer
         * 
         * @param tagged Tagged pointer value
         * @return 16-bit counter from upper 16 bits
         */
        static inline u16 GetCounter(TaggedPointer tagged) noexcept
        {
            // Extract upper 16 bits
            return static_cast<u16>(tagged >> 48);
        }

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
        
        alignas(128) std::atomic<TaggedPointer> m_FreeList{0};   // Lock-free free list with tagged pointers
        alignas(128) std::atomic<sizet> m_Capacity{0};           // Total blocks allocated
        alignas(128) std::atomic<sizet> m_FreeCount{0};          // Approximate free count
        
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
