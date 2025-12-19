// TaskConcurrencyLimiter.h - Limits concurrent task execution
// Ported from UE5.7 Tasks/TaskConcurrencyLimiter.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/HAL/EventPool.h"
#include "OloEngine/Memory/LockFreeList.h"
#include "OloEngine/Templates/SharedPointer.h"
#include "OloEngine/Templates/FunctionRef.h"
#include "OloEngine/Containers/Array.h"

#include <atomic_queue/atomic_queue.h>

#include <atomic>
#include <memory>

namespace OloEngine::Tasks
{
    namespace Private
    {
        // @class FConcurrencySlots
        // @brief A bounded lock-free FIFO queue of free slots in range [0 .. max_concurrency)
        //
        // This uses atomic_queue::AtomicQueueB<uint32> matching UE5.7's implementation.
        // FIFO ordering ensures fair slot acquisition under contention (unlike LIFO which
        // can cause starvation).
        //
        // Initially contains all slots in the range [0, max_concurrency).
        // Used to limit how many tasks can run concurrently.
        class FConcurrencySlots
        {
          public:
            explicit FConcurrencySlots(u32 MaxConcurrency)
                : FreeSlots(MaxConcurrency)
            {
                // Initialize with all slots - offset by IndexOffset since queue uses 0 as null
                for (u32 Index = IndexOffset; Index < MaxConcurrency + IndexOffset; ++Index)
                {
                    FreeSlots.push(Index);
                }
            }

            // @brief Try to allocate a slot (lock-free FIFO)
            // @param OutSlot Receives the allocated slot index
            // @return true if a slot was allocated
            bool Alloc(u32& OutSlot)
            {
                if (FreeSlots.try_pop(OutSlot))
                {
                    OutSlot -= IndexOffset;
                    return true;
                }
                return false;
            }

            // @brief Release a previously allocated slot (lock-free FIFO)
            void Release(u32 Slot)
            {
                FreeSlots.push(Slot + IndexOffset);
            }

          private:
            // This queue uses 0 as a special "null" value. To work around this,
            // slots are shifted by one for storage, ending up in [1 .. max_concurrency] range
            static constexpr u32 IndexOffset = 1;

            // Bounded lock-free FIFO queue (matching UE5.7's atomic_queue usage)
            atomic_queue::AtomicQueueB<u32> FreeSlots;
        };

        // @class FTaskConcurrencyLimiterImpl
        // @brief Lock-free implementation of FTaskConcurrencyLimiter
        //
        // Uses lock-free lists for both slot allocation and work queue.
        class FTaskConcurrencyLimiterImpl : public TSharedFromThis<FTaskConcurrencyLimiterImpl>
        {
          public:
            explicit FTaskConcurrencyLimiterImpl(u32 InMaxConcurrency, LowLevelTasks::ETaskPriority InTaskPriority)
                : m_ConcurrencySlots(InMaxConcurrency), m_TaskPriority(InTaskPriority)
            {
            }

            ~FTaskConcurrencyLimiterImpl()
            {
                // UE5.7 explicitly supports being destroyed before tasks complete.
                // The shared_ptr capture in task lambdas keeps the Pimpl alive
                // until all tasks finish executing. Do NOT wait here - it defeats
                // the fire-and-forget pattern.

                // Clean up any leftover completion event
                if (FEvent* Event = m_CompletionEvent.exchange(nullptr, std::memory_order_relaxed))
                {
                    TEventPool<EEventMode::ManualReset>::Get().ReturnToPool(Event);
                }
            }

            // @brief Push a new task into the limiter (lock-free)
            template<typename TaskFunctionType>
            void Push(const char* DebugName, TaskFunctionType&& TaskFunction)
            {
                // Create a wrapper that captures the task function
                TSharedPtr<LowLevelTasks::FTask> Task = MakeShared<LowLevelTasks::FTask>();

                Task->Init(
                    DebugName,
                    m_TaskPriority,
                    [TaskFunction = MoveTemp(TaskFunction),
                     this,
                     Pimpl = AsShared(), // to keep it alive
                     Task                // self-destruct
                ]()
                    {
                        // We can't pass the ConcurrencySlot in the lambda during creation as
                        // it's not actually acquired yet. The value will be passed using
                        // the user data when the task is launched.
                        u32 ConcurrencySlot = static_cast<u32>(reinterpret_cast<uptr>(Task->GetUserData()));

                        TaskFunction(ConcurrencySlot);
                        CompleteWorkItem(ConcurrencySlot);
                    });

                AddWorkItem(Task.Get());
            }

            // @brief Wait for all tasks to complete
            // @param Timeout Maximum time to wait
            // @return true if all tasks completed, false on timeout
            bool Wait(FMonotonicTimeSpan Timeout)
            {
                // Relaxed ordering is sufficient here since we're just checking if work is pending.
                // The actual synchronization happens through the event mechanism.
                if (m_NumWorkItems.load(std::memory_order_relaxed) == 0)
                {
                    return true;
                }

                // Lazy event allocation using compare_exchange (lock-free)
                FEvent* LocalCompletionEvent = m_CompletionEvent.load(std::memory_order_acquire);
                if (LocalCompletionEvent == nullptr)
                {
                    FEvent* NewEvent = TEventPool<EEventMode::ManualReset>::Get().GetEventFromPool();
                    if (!m_CompletionEvent.compare_exchange_strong(LocalCompletionEvent, NewEvent,
                                                                   std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // Another thread beat us - discard our event
                        TEventPool<EEventMode::ManualReset>::Get().ReturnToPool(NewEvent);
                    }
                    else
                    {
                        LocalCompletionEvent = NewEvent;
                    }
                }

                // Double-check after event allocation
                if (m_NumWorkItems.load(std::memory_order_acquire) == 0)
                {
                    return true;
                }

                // Wait on the event
                if (Timeout == FMonotonicTimeSpan::Infinity())
                {
                    return LocalCompletionEvent->Wait(); // Infinite wait
                }
                // Convert timeout to milliseconds for FEvent::Wait
                return LocalCompletionEvent->Wait(
                    static_cast<u32>(Timeout.ToMilliseconds()));
            }

          private:
            void AddWorkItem(LowLevelTasks::FTask* Task)
            {
                m_NumWorkItems.fetch_add(1, std::memory_order_acquire);

                // Push to work queue (lock-free)
                m_WorkQueue.Push(Task);

                // Try to acquire a slot and process
                u32 ConcurrencySlot;
                if (m_ConcurrencySlots.Alloc(ConcurrencySlot))
                {
                    ProcessQueueFromPush(ConcurrencySlot);
                }
            }

            void ProcessQueue(u32 ConcurrencySlot, bool bSkipFirstWakeUp)
            {
                bool bWakeUpWorker = !bSkipFirstWakeUp;
                do
                {
                    if (LowLevelTasks::FTask* Task = m_WorkQueue.Pop())
                    {
                        // Now that we know the ConcurrencySlot, set it at launch time so
                        // the executor can retrieve it.
                        Task->SetUserData(reinterpret_cast<void*>(static_cast<uptr>(ConcurrencySlot)));

                        LowLevelTasks::TryLaunch(*Task,
                                                 bWakeUpWorker ? LowLevelTasks::EQueuePreference::GlobalQueuePreference
                                                               : LowLevelTasks::EQueuePreference::LocalQueuePreference,
                                                 bWakeUpWorker);
                    }
                    else
                    {
                        // No more work - release slot
                        m_ConcurrencySlots.Release(ConcurrencySlot);
                        break;
                    }

                    // Don't skip wake-up if we launch any additional tasks
                    bWakeUpWorker = true;

                } while (m_ConcurrencySlots.Alloc(ConcurrencySlot));
            }

            void ProcessQueueFromWorker(u32 ConcurrencySlot)
            {
                // Once we are in a worker thread, we want to schedule on the local queue without waking up
                // additional workers to allow our own worker to pick up the next item and avoid wake-up cost.
                static constexpr bool bSkipFirstWakeUp = true;
                ProcessQueue(ConcurrencySlot, bSkipFirstWakeUp);
            }

            void ProcessQueueFromPush(u32 ConcurrencySlot)
            {
                // When we push new items, we don't want to skip any wake-up.
                static constexpr bool bSkipFirstWakeUp = false;
                ProcessQueue(ConcurrencySlot, bSkipFirstWakeUp);
            }

            void CompleteWorkItem(u32 ConcurrencySlot)
            {
                if (m_NumWorkItems.fetch_sub(1, std::memory_order_release) == 1)
                {
                    // Count went from 1 to 0 - signal completion
                    if (FEvent* LocalCompletionEvent = m_CompletionEvent.load(std::memory_order_acquire))
                    {
                        LocalCompletionEvent->Trigger();
                    }
                }

                ProcessQueueFromWorker(ConcurrencySlot);
            }

          private:
            FConcurrencySlots m_ConcurrencySlots;
            LowLevelTasks::ETaskPriority m_TaskPriority;

            // Lock-free FIFO work queue
            TLockFreePointerListFIFO<LowLevelTasks::FTask, OLO_PLATFORM_CACHE_LINE_SIZE> m_WorkQueue;

            std::atomic<u32> m_NumWorkItems{ 0 };
            std::atomic<FEvent*> m_CompletionEvent{ nullptr }; // Lazy-allocated event
        };

    } // namespace Private

    // @class FTaskConcurrencyLimiter
    // @brief A lightweight lock-free construct that limits the concurrency of tasks pushed into it
    //
    // This is useful when you have many tasks that access a shared resource and want
    // to limit how many can run at the same time. Each task receives a "slot" index
    // that can be used to index into per-slot buffers.
    //
    // @note This class supports being destroyed before the tasks it contains are finished.
    // @note This implementation is lock-free using indexed pointers with ABA counters.
    //
    // Example:
    // @code
    // // Allow at most 4 concurrent tasks
    // FTaskConcurrencyLimiter Limiter(4);
    //
    // // Per-slot accumulator buffers
    // std::array<int, 4> Accumulators = {};
    //
    // for (int i = 0; i < 1000; ++i)
    // {
    //     Limiter.Push("Accumulate", [&Accumulators, i](u32 Slot)
    //     {
    //         // Use Slot to index into a per-slot buffer without synchronization
    //     //     Accumulators[Slot] += i;
    //     // });
    // }
    //
    // Limiter.Wait();
    //
    // // Sum all per-slot accumulators
    // int Total = 0;
    // for (int Acc : Accumulators)
    // {
    //     Total += Acc;
    // }
    // @endcode
    // */
    class FTaskConcurrencyLimiter
    {
      public:
        // @brief Constructor
        // @param MaxConcurrency How wide the processing can go (number of concurrent tasks)
        // @param TaskPriority Priority the tasks will be launched with
        explicit FTaskConcurrencyLimiter(u32 MaxConcurrency,
                                         LowLevelTasks::ETaskPriority TaskPriority = LowLevelTasks::ETaskPriority::Default)
            : m_Impl(MakeShared<Private::FTaskConcurrencyLimiterImpl>(MaxConcurrency, TaskPriority))
        {
        }

        // @brief Push a new task
        //
        // @param DebugName Helps to identify the task in debugger and profiler
        // @param TaskFunction A callable with a slot parameter (u32). The slot parameter
        //                     is an index in [0..max_concurrency) range, unique at any
        //                     moment of time, that can be used to index a fixed-size buffer.
        template<typename TaskFunctionType>
        void Push(const char* DebugName, TaskFunctionType&& TaskFunction)
        {
            m_Impl->Push(DebugName, Forward<TaskFunctionType>(TaskFunction));
        }

        // @brief Wait for all tasks to complete
        //
        // @param Timeout Maximum amount of time to wait for tasks to finish
        // @return true if all tasks completed, false on timeout
        //
        // @note A wait is satisfied once the internal task counter reaches 0 and is
        //       never reset afterward when more tasks are added.
        bool Wait(FMonotonicTimeSpan Timeout = FMonotonicTimeSpan::Infinity())
        {
            return m_Impl->Wait(Timeout);
        }

      private:
        TSharedRef<Private::FTaskConcurrencyLimiterImpl> m_Impl;
    };

} // namespace OloEngine::Tasks
