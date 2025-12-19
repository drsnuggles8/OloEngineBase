// LocalWorkQueue.h - Local work queue for task parallelism
// Ported from UE5.7 Async/LocalWorkQueue.h

#pragma once

// @file LocalWorkQueue.h
// @brief Local work queue for work-stealing task parallelism
//
// TLocalWorkQueue provides a pattern for parallel task execution where:
// - A main thread creates initial work items
// - Worker tasks can be spawned to process items concurrently
// - Workers can add more work items as they discover them
// - The main thread runs until all work is complete
//
// Ported from Unreal Engine 5.7

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/FAAArrayQueue.h"
#include "OloEngine/HAL/EventCount.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Templates/RefCounting.h"
#include "OloEngine/Templates/FunctionRef.h"
#include "OloEngine/Memory/ConcurrentLinearAllocator.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>
#include <memory>

namespace OloEngine
{
    // @class TYCombinator
    // @brief Y-combinator for creating recursive lambdas
    //
    // Enables writing recursive lambdas by passing the combinator as the first argument.
    //
    // Example:
    // @code
    //     auto Factorial = MakeYCombinator([](auto Self, int N) -> int {
    //         return N <= 1 ? 1 : N * Self(N - 1);
    //     });
    //     int Result = Factorial(5);  // 120
    // @endcode
    //
    // @tparam LAMBDA The lambda type
    template<typename LAMBDA>
    class TYCombinator
    {
        LAMBDA Lambda;

      public:
        constexpr TYCombinator(LAMBDA&& InLambda)
            : Lambda(MoveTemp(InLambda))
        {
        }

        constexpr TYCombinator(const LAMBDA& InLambda)
            : Lambda(InLambda)
        {
        }

        template<typename... ARGS>
        constexpr auto operator()(ARGS&&... Args) const
            -> decltype(Lambda(static_cast<const TYCombinator<LAMBDA>&>(*this), Forward<ARGS>(Args)...))
        {
            return Lambda(static_cast<const TYCombinator<LAMBDA>&>(*this), Forward<ARGS>(Args)...);
        }

        template<typename... ARGS>
        constexpr auto operator()(ARGS&&... Args)
            -> decltype(Lambda(static_cast<TYCombinator<LAMBDA>&>(*this), Forward<ARGS>(Args)...))
        {
            return Lambda(static_cast<TYCombinator<LAMBDA>&>(*this), Forward<ARGS>(Args)...);
        }
    };

    // @brief Create a Y-combinator from a lambda
    // @param Lambda The lambda to wrap
    // @return A Y-combinator that can be called recursively
    template<typename LAMBDA>
    constexpr auto MakeYCombinator(LAMBDA&& Lambda)
    {
        return TYCombinator<std::decay_t<LAMBDA>>(Forward<LAMBDA>(Lambda));
    }

    // @class TLocalWorkQueue
    // @brief A work queue for parallel task execution with work-stealing
    //
    // This class provides a mechanism for parallel work distribution where:
    // - The main thread creates an initial work item
    // - Additional workers can be spawned to process work concurrently
    // - Any thread (main or worker) can add new work items
    // - The main thread runs until all work is complete
    //
    // Usage:
    // @code
    //     struct FMyTask { int Data; };
    //
    //     FMyTask InitialTask{42};
    //     TLocalWorkQueue<FMyTask> WorkQueue(&InitialTask, LowLevelTasks::ETaskPriority::Normal);
    //
    //     // Add more workers if needed
    //     WorkQueue.AddWorkers(4);
    //
    //     // Run until all work is done
    //     WorkQueue.Run([&](FMyTask* Task) {
    //         ProcessTask(Task);
    //         // Can add more work: WorkQueue.AddTask(NewTask);
    //     });
    // @endcode
    //
    // @tparam TaskType The type of work items (must be a pointer-like type)
    template<typename TaskType>
    class TLocalWorkQueue
    {
        OLO_NONCOPYABLE(TLocalWorkQueue);

        // @struct FInternalData
        // @brief Shared internal state for the work queue
        //
        // Reference-counted to ensure lifetime extends beyond the TLocalWorkQueue
        // if workers are still running when Run() returns.
        //
        // @note UE5.7 also inherits from TConcurrentLinearObject<FInternalData, FTaskGraphBlockAllocationTag>
        // for pooled allocation. This is a potential optimization for high-frequency
        // TLocalWorkQueue creation/destruction scenarios.
        struct FInternalData : public FThreadSafeRefCountedObject
        {
            FAAArrayQueue<TaskType> TaskQueue;
            std::atomic_int ActiveWorkers{ 0 };
            std::atomic_bool CheckDone{ false };
            FEventCount FinishedEvent;

            ~FInternalData()
            {
                OLO_CORE_ASSERT(ActiveWorkers == 0, "Workers still active at destruction");
                OLO_CORE_ASSERT(TaskQueue.Dequeue() == nullptr, "Queue not empty at destruction");
            }
        };

        TRefCountPtr<FInternalData> m_InternalData;
        LowLevelTasks::ETaskPriority m_Priority;
        TFunctionRef<void(TaskType*)>* m_DoWork = nullptr;

      public:
        // @brief Construct a local work queue with initial work
        // @param InitialWork The first work item to process
        // @param InPriority Task priority (Count = inherit from current task)
        TLocalWorkQueue(TaskType* InitialWork, LowLevelTasks::ETaskPriority InPriority = LowLevelTasks::ETaskPriority::Count)
            : m_Priority(InPriority)
        {
            // Determine effective priority
            if (m_Priority == LowLevelTasks::ETaskPriority::Count)
            {
                const LowLevelTasks::FTask* ActiveTask = LowLevelTasks::FTask::GetActiveTask();
                if (ActiveTask)
                {
                    m_Priority = ActiveTask->GetPriority();
                    // Bump background priority slightly for responsiveness
                    if (m_Priority == LowLevelTasks::ETaskPriority::BackgroundLow)
                    {
                        m_Priority = LowLevelTasks::ETaskPriority::BackgroundNormal;
                    }
                    else if (m_Priority == LowLevelTasks::ETaskPriority::BackgroundNormal)
                    {
                        m_Priority = LowLevelTasks::ETaskPriority::BackgroundHigh;
                    }
                }
                else
                {
                    m_Priority = LowLevelTasks::ETaskPriority::Default;
                }
            }

            m_InternalData = new FInternalData();
            AddTask(InitialWork);
        }

        // @brief Add a new work item to the queue
        // @param NewWork The work item to add
        //
        // Can be called from any thread (main or worker).
        // Must not be called after Run() has started checking for completion.
        void AddTask(TaskType* NewWork)
        {
            OLO_CORE_ASSERT(!m_InternalData->CheckDone.load(std::memory_order_relaxed),
                            "Cannot add tasks after queue completion started");
            m_InternalData->TaskQueue.Enqueue(NewWork);
        }

        // @brief Spawn additional worker tasks
        // @param NumWorkers Number of workers to add
        //
        // Workers will dequeue and process items until the queue is empty.
        // Must be called after m_DoWork is set (i.e., from within Run()).
        void AddWorkers(u16 NumWorkers)
        {
            OLO_CORE_ASSERT(!m_InternalData->CheckDone.load(std::memory_order_relaxed),
                            "Cannot add workers after queue completion started");
            OLO_CORE_ASSERT(m_DoWork != nullptr, "AddWorkers must be called from within Run()");

            for (u16 Index = 0; Index < NumWorkers; ++Index)
            {
                using FTaskHandle = TSharedPtr<LowLevelTasks::FTask, ESPMode::ThreadSafe>;
                FTaskHandle TaskHandle = MakeShared<LowLevelTasks::FTask, ESPMode::ThreadSafe>();

                TFunctionRef<void(TaskType*)>* LocalDoWork = m_DoWork;
                TRefCountPtr<FInternalData> InternalData = m_InternalData;

                TaskHandle->Init(TEXT("TLocalWorkQueue::AddWorkers"), m_Priority,
                                 [LocalDoWork, InternalData, TaskHandle]()
                                 {
                                     OLO_PROFILE_SCOPE("TLocalWorkQueue::Worker");

                                     InternalData->ActiveWorkers.fetch_add(1, std::memory_order_acquire);

                                     while (TaskType* Work = InternalData->TaskQueue.Dequeue())
                                     {
                                         OLO_CORE_ASSERT(!InternalData->CheckDone.load(std::memory_order_relaxed),
                                                         "Processing work after completion flag set");
                                         (*LocalDoWork)(Work);
                                     }

                                     if (InternalData->ActiveWorkers.fetch_sub(1, std::memory_order_release) == 1)
                                     {
                                         InternalData->FinishedEvent.Notify();
                                     }
                                 });

                LowLevelTasks::TryLaunch(*TaskHandle, LowLevelTasks::EQueuePreference::GlobalQueuePreference);
            }
        }

        // @brief Run the work queue until all items are processed
        // @param InDoWork Callback to process each work item
        //
        // This method blocks until:
        // - The queue is empty AND
        // - All workers have finished
        //
        // The callback can add new work items via AddTask() and spawn
        // additional workers via AddWorkers().
        void Run(TFunctionRef<void(TaskType*)> InDoWork)
        {
            m_DoWork = &InDoWork;

            OLO_PROFILE_SCOPE("TLocalWorkQueue::Run");

            while (true)
            {
                bool bNoActiveWorkers = m_InternalData->ActiveWorkers.load(std::memory_order_acquire) == 0;

                if (TaskType* Work = m_InternalData->TaskQueue.Dequeue())
                {
                    InDoWork(Work);
                }
                else if (bNoActiveWorkers &&
                         m_InternalData->ActiveWorkers.load(std::memory_order_acquire) == 0)
                {
                    // Queue empty and no workers - we're done
                    break;
                }
                else
                {
                    // Queue empty but workers may add more - wait for them
                    auto Token = m_InternalData->FinishedEvent.PrepareWait();

                    if (m_InternalData->ActiveWorkers.load(std::memory_order_acquire) == 0)
                    {
                        // Workers finished between our check and PrepareWait
                        continue;
                    }

                    OLO_PROFILE_SCOPE("TLocalWorkQueue::WaitingForWorkers");
                    m_InternalData->FinishedEvent.Wait(Token);
                }
            }

            m_InternalData->CheckDone.store(true);
            OLO_CORE_ASSERT(m_InternalData->TaskQueue.Dequeue() == nullptr,
                            "Queue should be empty after Run() completes");
        }
    };

} // namespace OloEngine
