#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"
#include "Task.h"

#include <atomic>

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
         * @return True if push succeeded, false if queue is full or task is null
         */
        bool Push(Ref<Task> task);

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
         */
        struct TaggedPtr
        {
            u64 Value;
            
            TaggedPtr() : Value(0) {}
            explicit TaggedPtr(u64 value) : Value(value) {}
            TaggedPtr(Node* ptr, u16 tag) 
                : Value((static_cast<u64>(tag) << 48) | (reinterpret_cast<uintptr_t>(ptr) & 0xFFFFFFFFFFFFULL)) {}
            
            Node* GetPtr() const { return reinterpret_cast<Node*>(Value & 0xFFFFFFFFFFFFULL); }
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
