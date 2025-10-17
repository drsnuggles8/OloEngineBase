#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"
#include "Task.h"
#include "QueueError.h"

#include <atomic>
#include <bit>
#include <expected>

namespace OloEngine
{
    /**
     * @brief Lock-free MPMC (multi-producer, multi-consumer) intrusive queue for tasks
     * 
     * This is a global work queue that multiple threads can safely push to and pop from.
     * It uses an intrusive linked list approach where Task objects contain the next pointer.
     * 
     * Key features:
     * - Lock-free push and pop operations
     * - FIFO ordering (tasks are popped in the order they were pushed)
     * - ABA problem prevention using tagged pointers (generation counter)
     * - Cache-line alignment to prevent false sharing
     * 
     * Implementation based on Michael-Scott queue algorithm with modifications for
     * our use case (no dummy node, direct Task pointers).
     * 
     * Thread Safety: Safe for concurrent access from multiple threads
     */
    class GlobalWorkQueue
    {
    public:
        /**
         * @brief Queue node - stored directly in Task for zero-allocation intrusive linking
         */
        struct Node
        {
            std::atomic<Node*> Next{nullptr};
            Task* TaskPtr{nullptr};
            
            Node() = default;
            explicit Node(Task* task) : TaskPtr(task) {}
        };

        /**
         * @brief Construct a global work queue with specified capacity
         * @param maxNodes Maximum number of nodes in the queue (default: 4096)
         */
        explicit GlobalWorkQueue(u32 maxNodes = 4096);
        ~GlobalWorkQueue();

        /**
         * @brief Push a task to the tail of the queue (producer operation)
         * 
         * This is a lock-free operation that can be called from any thread.
         * Tasks are pushed to the tail and will be popped from the head (FIFO).
         * 
         * @param task Task to push (must not be null)
         * @return void on success, QueueError on failure
         * 
         * Error codes:
         * - QueueError::NullTask: task parameter is null
         * - QueueError::AllocationFailed: node pool is exhausted
         * 
         * Example usage:
         * @code
         * auto result = queue.Push(myTask);
         * if (!result) {
         *     OLO_CORE_ERROR("Failed to push task: {}", QueueErrorToString(result.error()));
         * }
         * @endcode
         */
        std::expected<void, QueueError> Push(Ref<Task> task);

        /**
         * @brief Pop a task from the head of the queue (consumer operation)
         * 
         * This is a lock-free operation that can be called from any thread.
         * Returns the oldest task in the queue (FIFO order).
         * 
         * @return Task if available, null Ref if queue is empty
         */
        Ref<Task> Pop();

        /**
         * @brief Check if queue is approximately empty
         * 
         * This is a relaxed check that may have false positives/negatives
         * due to concurrent modifications. Use only for heuristics.
         * 
         * @return True if queue appears empty
         */
        bool IsEmpty() const;

        /**
         * @brief Get approximate size of queue
         * 
         * This is a relaxed check that may be inaccurate due to concurrent
         * modifications. Use only for statistics/debugging.
         * 
         * @return Approximate number of items in queue
         */
        u32 ApproximateSize() const;

    private:
        /**
         * @brief Tagged pointer for ABA prevention
         * 
         * Packs a 48-bit pointer and 16-bit generation counter into 64 bits:
         * - Bits 0-47: Node pointer (48 bits sufficient for x64 user-space addresses)
         * - Bits 48-63: Generation counter (16 bits, wraps around after 65536 operations)
         * 
         * CURRENT IMPLEMENTATION: 64-bit CAS with tagged pointers
         * - Advantages: Portable, fast, standard C++ atomics
         * - Limitation: 16-bit tag can wrap after 65,536 operations (potential ABA if sustained > 10M ops/sec)
         * 
         * TODO: Future optimization for extreme high-throughput scenarios (optional):
         * Consider implementing DWCAS (Double-Width Compare-And-Swap) as a compile-time option:
         * 
         * DWCAS Approach (128-bit atomic operation):
         * - Use CMPXCHG16B on x86-64 (available since Core 2 Duo, ~2006)
         * - Use CASP on ARM64 (ARMv8.1+)
         * - Full 64-bit pointer + 64-bit counter (effectively infinite ABA protection)
         * - ~20-30% slower than 64-bit CAS but mathematically ABA-proof
         * - Requires 16-byte alignment and platform-specific intrinsics
         * 
         * Implementation strategy:
         * 1. Add #define OLO_USE_DWCAS_FOR_QUEUES in Base.h or CMake option
         * 2. Create TaggedPtr128 struct with alignas(16) and platform checks
         * 3. Use _InterlockedCompareExchange128() on MSVC
         * 4. Use __sync_bool_compare_and_swap() with __int128 on GCC/Clang
         * 5. Add runtime detection for CPU support (CPUID bit 13 for CMPXCHG16B)
         * 
         * Reference: Unreal Engine 5.7's TLockFreePointerListUnordered implementation
         * 
         * Decision criteria for implementation:
         * - Profile shows sustained queue throughput > 10M operations/second
         * - Targeting 64+ core systems with extreme contention
         * - Need mathematical guarantee of ABA-freedom
         * 
         * Current 16-bit tag is SUFFICIENT for typical game engine workloads.
         */
        struct TaggedPtr
        {
            u64 Value;
            
            TaggedPtr() : Value(0) {}
            explicit TaggedPtr(u64 value) : Value(value) {}
            TaggedPtr(Node* ptr, u16 tag) 
                : Value((static_cast<u64>(tag) << 48) | (std::bit_cast<uintptr_t>(ptr) & 0xFFFFFFFFFFFFULL)) {}
            
            Node* GetPtr() const { return std::bit_cast<Node*>(Value & 0xFFFFFFFFFFFFULL); }
            u16 GetTag() const { return static_cast<u16>(Value >> 48); }
            u16 GetNextTag() const { return static_cast<u16>((Value >> 48) + 1); }
        };

        // Cache-line aligned head and tail to prevent false sharing
        // Now using tagged pointers (64-bit) instead of raw pointers for ABA protection
        alignas(128) std::atomic<u64> m_Head{0};
        alignas(128) std::atomic<u64> m_Tail{0};
        alignas(128) std::atomic<u32> m_ApproximateCount{0};

        // Node pool for lock-free allocation
        u32 m_MaxNodes;                                       ///< Maximum nodes in pool
        Node* m_NodePool{nullptr};
        alignas(128) std::atomic<Node*> m_FreeList{nullptr};
        
        // Dummy node for Michael-Scott algorithm
        Node* m_DummyNode{nullptr};

        /**
         * @brief Allocate a node from the free list
         */
        Node* AllocateNode(Task* task);

        /**
         * @brief Return a node to the free list
         */
        void FreeNode(Node* node);
    };

} // namespace OloEngine
