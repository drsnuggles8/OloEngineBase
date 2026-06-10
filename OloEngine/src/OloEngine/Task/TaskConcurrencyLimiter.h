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
#include "OloEngine/Templates/RefCounting.h"
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
            // Reference-counted wrapper for FTask so the task's lifetime can be shared
            // between the worker thread that runs it and any thread that retracts it in
            // Wait(). NOTE: uses FThreadSafeRefCountedObject (the *atomic* base) because
            // the refcount is touched from multiple threads concurrently. UE5.8 derives
            // FLimiterTask from FRefCountedObject, but in UE5.8 FRefCountedObject was made
            // atomic and FThreadSafeRefCountedObject deprecated as an alias for it; this
            // engine is UE5.7-based, where FRefCountedObject is still the non-atomic legacy
            // type, so the equivalent here is FThreadSafeRefCountedObject.
            struct FLimiterTask : FThreadSafeRefCountedObject
            {
                LowLevelTasks::FTask Task;
            };

          public:
            explicit FTaskConcurrencyLimiterImpl(u32 InMaxConcurrency, LowLevelTasks::ETaskPriority InTaskPriority)
                : m_ConcurrencySlots(InMaxConcurrency), m_TaskPriority(InTaskPriority)
            {
                m_ScheduledTasks.SetNum(InMaxConcurrency);
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
                FLimiterTask* LimiterTask = new FLimiterTask();
                LowLevelTasks::FTask& Task = LimiterTask->Task;

                Task.Init(
                    DebugName,
                    m_TaskPriority,
                    [TaskFunction = MoveTemp(TaskFunction),
                     this,
                     // Keep the limiter alive as long as this task is alive.
                     Pimpl = AsShared(),
                     // Keep the FLimiterTask wrapper alive as long as its inner Task is
                     // alive (TRefCountPtr AddRefs on construction: 0 -> 1).
                     LimiterTask = TRefCountPtr<FLimiterTask>(LimiterTask)]()
                    {
                        // The slot isn't known at creation; it's passed via the task's
                        // user data when the task is launched in ProcessQueue.
                        u32 ConcurrencySlot = static_cast<u32>(reinterpret_cast<uptr>(LimiterTask->Task.GetUserData()));

                        // Remove ourselves from ScheduledTasks as soon as we start, so the
                        // wait-thread doesn't pointlessly retract us. Whoever wins the
                        // exchange owns the AddRef done in ProcessQueue and must Release it.
                        // If Wait() already retracted us, the exchange returns null here.
                        bool bOwnLimiterTask = m_ScheduledTasks[ConcurrencySlot].Task.exchange(nullptr, std::memory_order_release) != nullptr;
                        if (bOwnLimiterTask)
                        {
                            LimiterTask->Release();
                        }

                        TaskFunction(ConcurrencySlot);
                        CompleteWorkItem(ConcurrencySlot);
                    });

                AddWorkItem(LimiterTask);
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

                // Drive queued-but-not-started tasks to completion on this thread via the
                // scheduler's retraction feature. All scheduler workers might be busy on
                // another subsystem's tasks (e.g. a ParallelFor) and unable to run ours; if
                // those tasks block on a resource this waiting thread holds, we'd deadlock.
                // We wait until a task has actually been launched before retracting it (a
                // slight wastage) so we don't have to duplicate ProcessQueue here.
                // (Matches UE5.8 FTaskConcurrencyLimiter::Wait.)
                // Honor a finite Timeout. If a full pass retracts nothing (bDidSomething ==
                // false) but tasks are still in flight on other workers (NumWorkItems != 0),
                // the loop would otherwise busy-spin re-scanning the slots, ignoring the
                // caller's deadline. Capture the deadline once and stop spinning when it
                // elapses with no progress, falling through to the timed event wait below.
                const FMonotonicTimePoint Deadline = Timeout.IsInfinity()
                                                         ? FMonotonicTimePoint::Infinity()
                                                         : FMonotonicTimePoint::Now() + Timeout;
                bool bDidSomething;
                do
                {
                    bDidSomething = false;
                    for (i32 SlotIndex = 0; SlotIndex < m_ScheduledTasks.Num(); ++SlotIndex)
                    {
                        // The slot is null while a task executes; if we claim a pointer we
                        // own the AddRef done in ProcessQueue and must Release it (and are
                        // now responsible for driving the task to completion).
                        if (FLimiterTask* LimiterTask = m_ScheduledTasks[SlotIndex].Task.exchange(nullptr, std::memory_order_acquire))
                        {
                            bDidSomething = true;
                            // We populate ScheduledTasks before launching, so we may have
                            // claimed a task that hasn't launched yet: TryExecute runs it
                            // directly; if it's already scheduled, TryExpedite bumps it.
                            if (!LimiterTask->Task.TryExecute())
                            {
                                LimiterTask->Task.TryExpedite();
                            }
                            LimiterTask->Release();
                        }
                    }

                    // While we keep retracting we keep going (deadlock-avoidance is active).
                    // Once a pass finds nothing retractable, stop spinning if the caller's
                    // deadline has elapsed; the remaining in-flight work is covered by the
                    // timed event wait below.
                    if (!bDidSomething && !Deadline.IsInfinity() && FMonotonicTimePoint::Now() >= Deadline)
                    {
                        break;
                    }

                    // Keep retracting until every pushed item has completed; stopping at the
                    // first empty pass could deadlock if a worker finishes our task, queues
                    // the next, then gets pulled onto another system before starting it.
                } while (bDidSomething || m_NumWorkItems.load(std::memory_order_acquire) != 0);

                // Wait on the event (should fall through immediately when the drain above ran
                // the last task and CompleteWorkItem triggered the event).
                if (Timeout.IsInfinity())
                {
                    return LocalCompletionEvent->Wait(); // Infinite wait
                }
                // Wait only for the time remaining until the deadline; a non-positive
                // remainder means the deadline already passed, so poll once (Wait(0)).
                const f64 RemainingMs = (Deadline - FMonotonicTimePoint::Now()).ToMilliseconds();
                return LocalCompletionEvent->Wait(RemainingMs > 0.0 ? static_cast<u32>(RemainingMs) : 0);
            }

          private:
            void AddWorkItem(FLimiterTask* Task)
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
                    if (FLimiterTask* LimiterTask = m_WorkQueue.Pop())
                    {
                        LowLevelTasks::FTask& Task = LimiterTask->Task;

                        // Now that we know the ConcurrencySlot, set it at launch time so
                        // the executor can retrieve it.
                        Task.SetUserData(reinterpret_cast<void*>(static_cast<uptr>(ConcurrencySlot)));

                        // Hold a local reference across the store + TryLaunch below. Once we
                        // publish into ScheduledTasks, a concurrent Wait() can exchange the
                        // slot, TryExecute the task inline, and Release it — destroying the
                        // wrapper (and the FTask that `Task` references) before TryLaunch is
                        // done touching it. ASan caught exactly this as a heap-use-after-free.
                        // This keep-alive guarantees the task outlives the launch regardless
                        // of a racing retraction.
                        TRefCountPtr<FLimiterTask> KeepAlive(LimiterTask);

                        // Publish into ScheduledTasks before launching so Wait() can retract
                        // it. This AddRef is the slot's own reference, balanced by the Release
                        // on whoever claims the slot (the task as it starts, or Wait()
                        // retracting it). TryLaunch may fail if Wait() already executed it,
                        // which is fine.
                        LimiterTask->AddRef();
                        m_ScheduledTasks[ConcurrencySlot].Task.store(LimiterTask, std::memory_order_release);

                        LowLevelTasks::TryLaunch(Task,
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
                // On a worker thread we schedule onto the local queue without waking another
                // worker, so our own worker picks up the next item and avoids the wake-up
                // cost. We must check IsWorkerThread() because Wait()'s retraction can run
                // this on a non-worker thread, which cannot itself pick up a local-queue task.
                ProcessQueue(ConcurrencySlot, LowLevelTasks::FScheduler::Get().IsWorkerThread());
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
            // Cache-line padded so adjacent slots, read/written from different threads in
            // ProcessQueue and Wait(), don't false-share.
            struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) FPaddedSharedTask
            {
                std::atomic<FLimiterTask*> Task{ nullptr };
            };

            FConcurrencySlots m_ConcurrencySlots;
            LowLevelTasks::ETaskPriority m_TaskPriority;

            // Lock-free FIFO work queue
            TLockFreePointerListFIFO<FLimiterTask, OLO_PLATFORM_CACHE_LINE_SIZE> m_WorkQueue;

            // One slot per concurrency unit, holding the launched-but-not-yet-started task so
            // Wait() can retract it. Sized to MaxConcurrency in the constructor.
            TArray<FPaddedSharedTask> m_ScheduledTasks;

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
