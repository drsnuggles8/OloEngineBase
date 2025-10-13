#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"

#include <atomic>

namespace OloEngine
{
    /**
     * @brief Lock-free work-stealing deque (Chase-Lev algorithm variant)
     * 
     * High-performance local work queue for worker threads. Based on the Chase-Lev
     * work-stealing deque with UE5-style three-state slots to prevent ABA problems.
     * 
     * Thread Safety:
     * - Push/Pop: Owner thread only (no synchronization needed)
     * - Steal: Any thread (lock-free with atomic operations)
     * 
     * Performance Characteristics:
     * - Push: O(1) wait-free
     * - Pop: O(1) wait-free (owner thread)
     * - Steal: O(1) lock-free (thief threads)
     * - Fixed-size ring buffer (no dynamic allocation during operation)
     * 
     * Cache-Line Alignment:
     * - Head and Tail are on separate cache lines (128 bytes apart)
     * - Prevents false sharing between owner and thieves
     * - Each slot is cache-line aligned for optimal performance
     * 
     * @tparam NumItems Ring buffer capacity (must be power of 2)
     */
    template<u32 NumItems = 1024>
    class LocalWorkQueue
    {
        static_assert((NumItems & (NumItems - 1)) == 0, "NumItems must be power of 2");

    public:
        /**
         * @brief Slot state for ABA prevention
         * 
         * Three-state system prevents the ABA problem in work stealing:
         * - Free: Slot is empty and available
         * - Taken: Slot is being written to (transitional state)
         * - Item: Slot contains a valid task pointer
         */
        enum class ESlotState : u8
        {
            Free = 0,
            Taken = 1,
            Item = 2
        };

        /**
         * @brief Initialize empty queue
         */
        LocalWorkQueue()
            : m_Head(0)  // Head starts at 0
            , m_Tail(0)  // Tail starts at 0 (empty when head == tail)
        {
            // Initialize all slots to Free state
            for (u32 i = 0; i < NumItems; ++i)
            {
                m_ItemSlots[i].Value.store(EncodeSlot(nullptr, ESlotState::Free), 
                                          std::memory_order_relaxed);
            }
        }

        /**
         * @brief Push task onto queue (owner thread only)
         * 
         * FIFO from owner's perspective. Pushes to the head of the deque.
         * This is a wait-free operation for the owner thread.
         * 
         * @param task Task to push (must not be nullptr)
         * @return True if pushed successfully, false if queue is full
         */
        bool Push(Ref<Task> task)
        {
            OLO_PROFILE_FUNCTION();
            
            OLO_CORE_ASSERT(task, "Cannot push null task");

            // Read current head (owner thread only, no synchronization)
            const u32 currentHead = m_Head;
            
            // Read tail with acquire to see stealer's updates
            const u32 currentTail = m_Tail.load(std::memory_order_acquire);
            
            // Calculate size (wrapping correctly)
            const u32 size = (currentHead >= currentTail) ? 
                (currentHead - currentTail) : 
                (NumItems - currentTail + currentHead);
            
            // Check if queue is full (leave one slot empty to distinguish full from empty)
            if (size >= NumItems - 1)
            {
                return false;
            }

            // Push at current head position
            u32 slotIndex = currentHead & (NumItems - 1);
            
            // Mark slot as Taken (prevents stealers from taking it prematurely)
            uintptr_t expected = EncodeSlot(nullptr, ESlotState::Free);
            uintptr_t desired = EncodeSlot(nullptr, ESlotState::Taken);
            
            bool success = m_ItemSlots[slotIndex].Value.compare_exchange_strong(
                expected, desired, 
                std::memory_order_acquire,
                std::memory_order_relaxed);

            if (!success)
            {
                // Slot wasn't free (shouldn't happen)
                return false;
            }

            // Increment reference count (task will be held by queue)
            task->IncRefCount();

            // Write the task pointer with release semantics
            // This ensures the task data is visible to stealers
            m_ItemSlots[slotIndex].Value.store(
                EncodeSlot(task.Raw(), ESlotState::Item),
                std::memory_order_release);

            // Update head (owner thread only, no synchronization needed)
            m_Head = currentHead + 1;

            return true;
        }

        /**
         * @brief Pop task from queue (owner thread only)
         * 
         * FIFO from owner's perspective. Pops from the head of the deque.
         * Returns the most recently pushed item.
         * 
         * @return Task if available, null Ref if queue is empty
         */
        Ref<Task> Pop()
        {
            OLO_PROFILE_FUNCTION();

            const u32 currentHead = m_Head;
            const u32 currentTail = m_Tail.load(std::memory_order_acquire);

            // Check if queue is empty
            if (currentHead == currentTail)
            {
                return nullptr;
            }

            // Decrement head first (pop from head - 1)
            const u32 newHead = currentHead - 1;
            m_Head = newHead;
            
            u32 slotIndex = newHead & (NumItems - 1);
            
            // Try to take the item
            uintptr_t slotValue = m_ItemSlots[slotIndex].Value.load(std::memory_order_acquire);
            ESlotState state = GetSlotState(slotValue);

            if (state != ESlotState::Item)
            {
                // Slot is empty or being stolen, restore head
                m_Head = currentHead;
                return nullptr;
            }

            // Check if this is the last item (might race with stealer)
            if (newHead == currentTail)
            {
                // Potential race with stealer - use CAS to claim it
                uintptr_t expected = slotValue;
                uintptr_t desired = EncodeSlot(nullptr, ESlotState::Taken);
                
                bool success = m_ItemSlots[slotIndex].Value.compare_exchange_strong(
                    expected, desired,
                    std::memory_order_acquire,
                    std::memory_order_relaxed);
                
                if (!success)
                {
                    // Stealer got it, restore head
                    m_Head = currentHead;
                    return nullptr;
                }
                
                // We got it - extract task pointer
                Task* taskPtr = GetSlotTask(expected);
                
                // Mark as free
                m_ItemSlots[slotIndex].Value.store(
                    EncodeSlot(nullptr, ESlotState::Free),
                    std::memory_order_release);

                // Return task - the reference from Push is transferred to caller
                // Ref constructor will increment, so decrement to compensate
                Ref<Task> result(taskPtr);
                if (taskPtr) taskPtr->DecRefCount();
                return result;
            }
            else
            {
                // Not the last item, no race possible - just take it
                Task* taskPtr = GetSlotTask(slotValue);
                
                // Mark as free
                m_ItemSlots[slotIndex].Value.store(
                    EncodeSlot(nullptr, ESlotState::Free),
                    std::memory_order_release);

                // Return task - the reference from Push is transferred to caller
                // Ref constructor will increment, so decrement to compensate
                Ref<Task> result(taskPtr);
                if (taskPtr) taskPtr->DecRefCount();
                return result;
            }
        }

        /**
         * @brief Steal task from queue (any thread)
         * 
         * LIFO from stealer's perspective. Steals from the tail of the deque
         * (oldest work first, better for cache locality of owner).
         * 
         * This is a lock-free operation that may contend with other stealers
         * and with the owner's Pop operation.
         * 
         * @return Task if stolen successfully, null Ref if queue is empty or contention
         */
        Ref<Task> Steal()
        {
            OLO_PROFILE_FUNCTION();

            // Read tail with acquire ordering
            u32 currentTail = m_Tail.load(std::memory_order_acquire);
            
            // Read head with acquire ordering to see owner's updates
            u32 currentHead = m_Head;  // Relaxed read OK, we'll verify with CAS

            // Quick empty check
            if (currentTail >= currentHead)
            {
                return nullptr;
            }

            u32 slotIndex = currentTail & (NumItems - 1);
            
            // Try to steal the item
            uintptr_t slotValue = m_ItemSlots[slotIndex].Value.load(std::memory_order_acquire);
            ESlotState state = GetSlotState(slotValue);

            if (state != ESlotState::Item)
            {
                // Slot is empty, taken, or being modified
                return nullptr;
            }

            // Try to atomically swap Item -> Taken
            uintptr_t expected = slotValue;
            uintptr_t desired = EncodeSlot(nullptr, ESlotState::Taken);
            
            bool success = m_ItemSlots[slotIndex].Value.compare_exchange_strong(
                expected, desired,
                std::memory_order_acquire,
                std::memory_order_relaxed);

            if (!success)
            {
                // Another stealer or owner got it
                return nullptr;
            }

            // Successfully stole the item - now update tail
            // Use CAS to ensure we only increment if nobody else did
            u32 expectedTail = currentTail;
            u32 desiredTail = currentTail + 1;
            
            bool tailSuccess = m_Tail.compare_exchange_strong(
                expectedTail, desiredTail,
                std::memory_order_release,
                std::memory_order_relaxed);

            if (!tailSuccess)
            {
                // Another stealer updated tail, put the item back and retry
                m_ItemSlots[slotIndex].Value.store(
                    expected,  // Restore the original value
                    std::memory_order_release);
                return nullptr;
            }

            // Extract task pointer
            Task* taskPtr = GetSlotTask(expected);
            
            // Mark as free
            m_ItemSlots[slotIndex].Value.store(
                EncodeSlot(nullptr, ESlotState::Free),
                std::memory_order_release);

            // Return stolen task (reference count already incremented during push)
            // Ref constructor will increment, so we need to decrement to compensate
            Ref<Task> result(taskPtr);
            taskPtr->DecRefCount();  // Balance the increment from Ref constructor
            return result;
        }

        /**
         * @brief Check if queue is empty
         * 
         * This is an approximate check that may have false positives/negatives
         * due to concurrent modifications.
         * 
         * @return True if queue appears empty
         */
        bool IsEmpty() const
        {
            const u32 currentHead = m_Head;
            const u32 currentTail = m_Tail.load(std::memory_order_relaxed);
            
            return currentHead == currentTail;
        }

        /**
         * @brief Get approximate queue size
         * 
         * May be inaccurate due to concurrent modifications.
         * 
         * @return Approximate number of items in queue
         */
        u32 ApproximateSize() const
        {
            const u32 currentHead = m_Head;
            const u32 currentTail = m_Tail.load(std::memory_order_relaxed);
            
            if (currentHead >= currentTail)
                return currentHead - currentTail;
            
            // Wrapped around
            return (NumItems - currentTail) + currentHead;
        }

    private:
        /**
         * @brief Encode task pointer and state into a single uintptr_t
         * 
         * Uses the low 2 bits for state (assumes task pointers are aligned to 4 bytes).
         * 
         * @param task Task pointer (may be null)
         * @param state Slot state
         * @return Encoded value
         */
        static uintptr_t EncodeSlot(Task* task, ESlotState state)
        {
            uintptr_t ptr = reinterpret_cast<uintptr_t>(task);
            OLO_CORE_ASSERT((ptr & 0x3) == 0, "Task pointer must be 4-byte aligned");
            return ptr | static_cast<uintptr_t>(state);
        }

        /**
         * @brief Extract task pointer from encoded slot value
         * 
         * @param encoded Encoded slot value
         * @return Task pointer (may be null)
         */
        static Task* GetSlotTask(uintptr_t encoded)
        {
            return reinterpret_cast<Task*>(encoded & ~uintptr_t(0x3));
        }

        /**
         * @brief Extract slot state from encoded value
         * 
         * @param encoded Encoded slot value
         * @return Slot state
         */
        static ESlotState GetSlotState(uintptr_t encoded)
        {
            return static_cast<ESlotState>(encoded & 0x3);
        }

    private:
        /**
         * @brief Cache-line aligned slot for lock-free operations
         */
        struct alignas(128) AlignedElement
        {
            std::atomic<uintptr_t> Value;
        };

        // Cache-line align head and tail to prevent false sharing
        alignas(128) u32 m_Head;                    ///< Owner thread only (FIFO push/pop)
        alignas(128) std::atomic<u32> m_Tail;       ///< Shared (stealers increment this)
        
        AlignedElement m_ItemSlots[NumItems];       ///< Ring buffer of slots
    };

} // namespace OloEngine
