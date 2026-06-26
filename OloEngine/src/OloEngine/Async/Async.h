// Async.h - High-level async execution helpers
// Ported from UE5.7 Async/Async.h

#pragma once

/**
 * @file Async.h
 * @brief Convenient functions for executing code asynchronously
 *
 * Provides easy-to-use Async(), AsyncThread(), and AsyncPool() functions
 * that return TFuture objects for the results.
 *
 * Ported from Unreal Engine's Async/Async.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Async/Future.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Templates/FunctionRef.h"

#include <atomic>
#include <type_traits>

namespace OloEngine
{
    // Forward declaration
    class FQueuedThreadPool;

    /**
     * @enum EAsyncExecution
     * @brief Enumerates available asynchronous execution methods
     */
    enum class EAsyncExecution
    {
        /** Execute on the task scheduler (for short running tasks). */
        TaskGraph,

        /** Execute in separate thread (for long running tasks). */
        Thread,

        /** Execute in global queued thread pool. */
        ThreadPool,
    };

    /**
     * @brief Helper to set promise value from a callable
     */
    template<typename ResultType, typename CallableType>
    inline void SetPromise(TPromise<ResultType>& Promise, CallableType&& Callable)
    {
        Promise.SetValue(Forward<CallableType>(Callable)());
    }

    template<typename CallableType>
    inline void SetPromise(TPromise<void>& Promise, CallableType&& Callable)
    {
        Forward<CallableType>(Callable)();
        Promise.SetValue();
    }

    /**
     * @class TAsyncRunnable
     * @brief Runnable that executes a function on a separate thread
     */
    template<typename ResultType>
    class TAsyncRunnable : public FRunnable
    {
      public:
        TAsyncRunnable(TUniqueFunction<ResultType()>&& InFunction, TPromise<ResultType>&& InPromise)
            : m_Function(MoveTemp(InFunction)), m_Promise(MoveTemp(InPromise))
        {
        }

        u32 Run() override
        {
            SetPromise(m_Promise, m_Function);

            // Mark as complete so the thread can be cleaned up
            m_Complete.store(true, std::memory_order_release);

            return 0;
        }

        void Exit() override
        {
            // Schedule self-destruction - the thread will clean up after itself
            // We need to delete the runnable after the thread is done
        }

        bool IsComplete() const
        {
            return m_Complete.load(std::memory_order_acquire);
        }

        void SetThread(FRunnableThread* InThread)
        {
            m_Thread = InThread;
        }

        FRunnableThread* GetThread() const
        {
            return m_Thread;
        }

      private:
        TUniqueFunction<ResultType()> m_Function;
        TPromise<ResultType> m_Promise;
        FRunnableThread* m_Thread = nullptr;
        std::atomic<bool> m_Complete{ false };
    };

    namespace Private
    {
        /**
         * @brief Generates unique thread indices for async threads
         */
        struct FAsyncThreadIndex
        {
            static i32 GetNext()
            {
                static std::atomic<i32> s_Counter{ 0 };
                return s_Counter.fetch_add(1, std::memory_order_relaxed);
            }
        };

        /**
         * @brief Heap owner for a fire-and-forget LowLevelTasks::FTask.
         *
         * Mirrors UE's Tasks::Private::FTaskBase, which owns its LowLevelTasks::FTask
         * as a member and is cleaned up via a LowLevelTasks::TDeleter<FTaskBase,
         * &FTaskBase::Release> captured by value in the task's runnable (see UE
         * Tasks/TaskPrivate.h). The deleter is destroyed together with the runnable —
         * which the scheduler does AFTER it flags the task Completed — so deletion
         * never trips ~FTask()'s IsCompleted() assertion. These tasks are fire-and-
         * forget (no TFuture/waiter holds a reference), so Release() is a plain delete
         * rather than the refcount decrement UE uses for referenced high-level tasks.
         */
        struct FFireAndForgetTask
        {
            LowLevelTasks::FTask Task;

            void Release()
            {
                delete this;
            }
        };

        /**
         * @brief Clean up completed async threads
         *
         * This is scheduled on the task graph to delete the runnable and thread
         * after execution completes.
         */
        template<typename ResultType>
        void CleanupAsyncThread(TAsyncRunnable<ResultType>* Runnable, FRunnableThread* Thread)
        {
            // Wait for the thread to finish
            if (Thread)
            {
                Thread->WaitForCompletion();
                delete Thread;
            }
            delete Runnable;
        }

    } // namespace Private

    /**
     * @brief Execute a function asynchronously
     *
     * Usage examples:
     * @code
     * // Using lambda
     * auto Result = Async(EAsyncExecution::TaskGraph, []() {
     *     return 123;
     * });
     * int Value = Result.Get(); // Blocks until complete
     *
     * // Fire and forget
     * Async(EAsyncExecution::Thread, []() {
     *     DoSomeLongRunningWork();
     * });
     * @endcode
     *
     * @param Execution The execution method to use
     * @param Callable The function to execute
     * @param CompletionCallback Optional callback when execution completes
     * @return A TFuture that will receive the return value
     */
    template<typename CallableType>
    auto Async(EAsyncExecution Execution, CallableType&& Callable,
               TUniqueFunction<void()> CompletionCallback = nullptr)
        -> TFuture<decltype(Forward<CallableType>(Callable)())>
    {
        using ResultType = decltype(Forward<CallableType>(Callable)());
        TUniqueFunction<ResultType()> Function(Forward<CallableType>(Callable));
        TPromise<ResultType> Promise;
        TFuture<ResultType> Future = Promise.GetFuture();

        switch (Execution)
        {
            case EAsyncExecution::TaskGraph:
            {
                // Launch on the task scheduler. The runnable owns the heap task and
                // cleans it up with a TDeleter captured by value, so it is deleted
                // after the scheduler marks it Completed, not from inside the body —
                // see AsyncTask() for why.
                Private::FFireAndForgetTask* Owner = new Private::FFireAndForgetTask();
                Owner->Task.Init(
                    "AsyncTask",
                    LowLevelTasks::ETaskPriority::Normal,
                    [Function = MoveTemp(Function), Promise = MoveTemp(Promise),
                     Callback = MoveTemp(CompletionCallback),
                     Deleter = LowLevelTasks::TDeleter<Private::FFireAndForgetTask, &Private::FFireAndForgetTask::Release>{ Owner }]() mutable
                    {
                        SetPromise(Promise, Function);
                        if (Callback)
                        {
                            Callback();
                        }
                    },
                    LowLevelTasks::ETaskFlags::DefaultFlags);
                LowLevelTasks::TryLaunch(Owner->Task);
            }
            break;

            case EAsyncExecution::Thread:
                if (FPlatformProcess::SupportsMultithreading())
                {
                    // Create a dedicated thread
                    auto* Runnable = new TAsyncRunnable<ResultType>(MoveTemp(Function), MoveTemp(Promise));

                    char ThreadName[64];
                    snprintf(ThreadName, sizeof(ThreadName), "TAsync %d", Private::FAsyncThreadIndex::GetNext());

                    FRunnableThread* Thread = FRunnableThread::Create(
                        Runnable,
                        ThreadName,
                        0, // Default stack size
                        EThreadPriority::TPri_Normal);

                    if (Thread)
                    {
                        Runnable->SetThread(Thread);

                        // Schedule cleanup on task graph after completion
                        // The thread will self-clean via the runnable's Exit().
                        // The cleanup task deletes itself via a TDeleter captured in
                        // its runnable (deleted after it is flagged Completed) — see
                        // AsyncTask() for why a body-side delete is unsafe.
                        Private::FFireAndForgetTask* CleanupOwner = new Private::FFireAndForgetTask();
                        CleanupOwner->Task.Init(
                            "AsyncCleanup",
                            LowLevelTasks::ETaskPriority::BackgroundLow,
                            [Runnable, Thread, Callback = MoveTemp(CompletionCallback),
                             Deleter = LowLevelTasks::TDeleter<Private::FFireAndForgetTask, &Private::FFireAndForgetTask::Release>{ CleanupOwner }]() mutable
                            {
                                // Wait for completion and clean up
                                Thread->WaitForCompletion();
                                delete Thread;
                                delete Runnable;
                                if (Callback)
                                {
                                    Callback();
                                }
                            },
                            LowLevelTasks::ETaskFlags::DefaultFlags);
                        LowLevelTasks::TryLaunch(CleanupOwner->Task);
                    }
                    else
                    {
                        // Thread creation failed, run synchronously
                        delete Runnable;
                        TPromise<ResultType> SyncPromise;
                        TFuture<ResultType> SyncFuture = SyncPromise.GetFuture();
                        SetPromise(SyncPromise, Function);
                        if (CompletionCallback)
                        {
                            CompletionCallback();
                        }
                        return SyncFuture;
                    }
                }
                else
                {
                    // No multithreading, run synchronously
                    SetPromise(Promise, Function);
                    if (CompletionCallback)
                    {
                        CompletionCallback();
                    }
                }
                break;

            case EAsyncExecution::ThreadPool:
                // ThreadPool requires FQueuedThreadPool - use AsyncPool() directly
                // Fall back to TaskGraph for now
                {
                    // Runnable owns the heap task and cleans it up with a TDeleter
                    // captured by value (deleted post-completion), not via a delete in
                    // the body — see AsyncTask() for why.
                    Private::FFireAndForgetTask* Owner = new Private::FFireAndForgetTask();
                    Owner->Task.Init(
                        "AsyncPoolTask",
                        LowLevelTasks::ETaskPriority::BackgroundNormal,
                        [Function = MoveTemp(Function), Promise = MoveTemp(Promise),
                         Callback = MoveTemp(CompletionCallback),
                         Deleter = LowLevelTasks::TDeleter<Private::FFireAndForgetTask, &Private::FFireAndForgetTask::Release>{ Owner }]() mutable
                        {
                            SetPromise(Promise, Function);
                            if (Callback)
                            {
                                Callback();
                            }
                        },
                        LowLevelTasks::ETaskFlags::DefaultFlags);
                    LowLevelTasks::TryLaunch(Owner->Task);
                }
                break;
        }

        return MoveTemp(Future);
    }

    /**
     * @brief Execute a function asynchronously using a separate thread
     *
     * @param Callable The function to execute
     * @param StackSize Stack size for the thread (0 = default)
     * @param ThreadPri Thread priority
     * @param CompletionCallback Optional callback when execution completes
     * @return A TFuture that will receive the return value
     */
    template<typename CallableType>
    auto AsyncThread(CallableType&& Callable,
                     u32 StackSize = 0,
                     EThreadPriority ThreadPri = EThreadPriority::TPri_Normal,
                     TUniqueFunction<void()> CompletionCallback = nullptr)
        -> TFuture<decltype(Forward<CallableType>(Callable)())>
    {
        using ResultType = decltype(Forward<CallableType>(Callable)());
        TUniqueFunction<ResultType()> Function(Forward<CallableType>(Callable));
        TPromise<ResultType> Promise;
        TFuture<ResultType> Future = Promise.GetFuture();

        if (FPlatformProcess::SupportsMultithreading())
        {
            auto* Runnable = new TAsyncRunnable<ResultType>(MoveTemp(Function), MoveTemp(Promise));

            char ThreadName[64];
            snprintf(ThreadName, sizeof(ThreadName), "TAsyncThread %d", Private::FAsyncThreadIndex::GetNext());

            FRunnableThread* Thread = FRunnableThread::Create(
                Runnable,
                ThreadName,
                StackSize,
                ThreadPri);

            if (Thread)
            {
                Runnable->SetThread(Thread);

                // Schedule cleanup. The cleanup task deletes itself via a TDeleter
                // captured in its runnable (deleted after it is flagged Completed) —
                // see AsyncTask() for why a body-side delete is unsafe.
                Private::FFireAndForgetTask* CleanupOwner = new Private::FFireAndForgetTask();
                CleanupOwner->Task.Init(
                    "AsyncThreadCleanup",
                    LowLevelTasks::ETaskPriority::BackgroundLow,
                    [Runnable, Thread, Callback = MoveTemp(CompletionCallback),
                     Deleter = LowLevelTasks::TDeleter<Private::FFireAndForgetTask, &Private::FFireAndForgetTask::Release>{ CleanupOwner }]() mutable
                    {
                        Thread->WaitForCompletion();
                        delete Thread;
                        delete Runnable;
                        if (Callback)
                        {
                            Callback();
                        }
                    },
                    LowLevelTasks::ETaskFlags::DefaultFlags);
                LowLevelTasks::TryLaunch(CleanupOwner->Task);
            }
            else
            {
                delete Runnable;
                // Run synchronously on failure
                TPromise<ResultType> SyncPromise;
                SetPromise(SyncPromise, Function);
                if (CompletionCallback)
                {
                    CompletionCallback();
                }
                return SyncPromise.GetFuture();
            }
        }
        else
        {
            SetPromise(Promise, Function);
            if (CompletionCallback)
            {
                CompletionCallback();
            }
        }

        return MoveTemp(Future);
    }

    /**
     * @brief Execute a task on the task scheduler
     *
     * Convenience function to quickly launch a task without creating
     * a promise/future pair.
     *
     * @param Priority Task priority
     * @param Function The function to execute
     */
    inline void AsyncTask(LowLevelTasks::ETaskPriority Priority, TUniqueFunction<void()> Function)
    {
        // Own the heap task and clean it up with a TDeleter captured in the runnable,
        // exactly as UE's FTaskBase does (Tasks/TaskPrivate.h). The deleter fires when
        // the runnable is destroyed — which the scheduler does AFTER flagging the task
        // Completed — so deletion never trips ~FTask()'s IsCompleted() assertion.
        // (Deleting the task from inside the runnable body would run while it is still
        // Running and crash scheduler teardown / StopWorkers.)
        Private::FFireAndForgetTask* Owner = new Private::FFireAndForgetTask();
        Owner->Task.Init(
            "AsyncTask",
            Priority,
            [Function = MoveTemp(Function),
             Deleter = LowLevelTasks::TDeleter<Private::FFireAndForgetTask, &Private::FFireAndForgetTask::Release>{ Owner }]() mutable
            {
                Function();
            },
            LowLevelTasks::ETaskFlags::DefaultFlags);
        LowLevelTasks::TryLaunch(Owner->Task);
    }

    /**
     * @brief Execute a task with default priority
     */
    inline void AsyncTask(TUniqueFunction<void()> Function)
    {
        AsyncTask(LowLevelTasks::ETaskPriority::Normal, MoveTemp(Function));
    }

} // namespace OloEngine
