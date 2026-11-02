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
#include "OloEngine/Templates/Function.h"

#include <atomic>
#include <cstdio>
#include <memory>
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
        TAsyncRunnable(TUniqueFunction<ResultType()>&& InFunction, TPromise<ResultType>&& InPromise,
                       TUniqueFunction<void()> InCompletionCallback = nullptr)
            : m_Function(MoveTemp(InFunction)), m_Promise(MoveTemp(InPromise)),
              m_CompletionCallback(MoveTemp(InCompletionCallback))
        {
        }

        u32 Run() override
        {
            SetPromise(m_Promise, m_Function);
            m_Complete.store(true, std::memory_order_release);

            // The result is already published. The callback runs on this
            // dedicated thread (or the caller on synchronous startup fallback),
            // while the callable's captures are still alive. It never needs a
            // scheduler worker to make progress.
            if (m_CompletionCallback)
                m_CompletionCallback();

            return 0;
        }

        void Exit() override
        {
            // CreateAutoDelete releases us after the native entry point finishes
            // its remaining thread-state and TLS accesses.
        }

        bool IsComplete() const
        {
            return m_Complete.load(std::memory_order_acquire);
        }

      private:
        TUniqueFunction<ResultType()> m_Function;
        TPromise<ResultType> m_Promise;
        TUniqueFunction<void()> m_CompletionCallback;
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
         * as a member and is released via a TDeleter captured in the runnable. These
         * tasks are fire-and-forget (no TFuture/waiter holds a reference), so Release()
         * is a plain delete rather than the refcount decrement UE uses for referenced
         * high-level tasks. Use LaunchFireAndForget() rather than touching this directly.
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
         * @brief Launch a fire-and-forget task on the low-level scheduler.
         *
         * The heap task owns itself through a LowLevelTasks::TDeleter captured by value
         * in its runnable, so it is freed when the runnable is destroyed — which the
         * scheduler does AFTER flagging the task Completed (FTask::ExecuteTask moves the
         * runnable out, runs it, sets CompletedFlag, then destroys the moved runnable).
         * Deleting the task from inside the body instead runs while it is still Running,
         * tripping ~FTask()'s IsCompleted() assertion and handing the scheduler a freed
         * task — which crashes StopWorkers / scheduler teardown. This mirrors UE's
         * FTaskBase / TDeleter<FTaskBase, &FTaskBase::Release> ownership
         * (Tasks/TaskPrivate.h); every fire-and-forget launch must route through here so
         * that contract lives in exactly one place.
         */
        inline void LaunchFireAndForget(const char* DebugName, LowLevelTasks::ETaskPriority Priority, TUniqueFunction<void()> Runnable)
        {
            FFireAndForgetTask* Owner = new FFireAndForgetTask();
            Owner->Task.Init(
                DebugName,
                Priority,
                [Runnable = MoveTemp(Runnable),
                 Deleter = LowLevelTasks::TDeleter<FFireAndForgetTask, &FFireAndForgetTask::Release>{ Owner }]() mutable
                {
                    Runnable();
                },
                LowLevelTasks::ETaskFlags::DefaultFlags);
            LowLevelTasks::TryLaunch(Owner->Task);
        }

        // Both public dedicated-thread APIs use this ownership boundary. Keeping
        // the original promise in the runnable also makes a native-start failure
        // safe for move-only callables: the still-owned runnable executes inline.
        template<typename ResultType, typename ThreadFactory = decltype(&FRunnableThread::CreateAutoDelete)>
        void LaunchAsyncThread(TUniqueFunction<ResultType()>&& Function,
                               TPromise<ResultType>&& Promise,
                               TUniqueFunction<void()>&& CompletionCallback,
                               const char* ThreadName, u32 StackSize, EThreadPriority ThreadPri,
                               ThreadFactory CreateThread = &FRunnableThread::CreateAutoDelete)
        {
            auto Runnable = std::make_unique<TAsyncRunnable<ResultType>>(
                MoveTemp(Function), MoveTemp(Promise), MoveTemp(CompletionCallback));
            if (FPlatformProcess::SupportsMultithreading() &&
                CreateThread(Runnable.get(), ThreadName, StackSize, ThreadPri,
                             0, EThreadCreateFlags::None))
            {
                // The auto-delete thread can already have completed, so do not
                // dereference Runnable after transferring ownership.
                (void)Runnable.release();
            }
            else
            {
                Runnable->Run();
            }
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
     *        Dedicated-thread callbacks run after the result on that thread,
     *        before callable captures are destroyed; startup fallback runs inline.
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
                Private::LaunchFireAndForget(
                    "AsyncTask",
                    LowLevelTasks::ETaskPriority::Normal,
                    [Function = MoveTemp(Function), Promise = MoveTemp(Promise),
                     Callback = MoveTemp(CompletionCallback)]() mutable
                    {
                        SetPromise(Promise, Function);
                        if (Callback)
                        {
                            Callback();
                        }
                    });
            }
            break;

            case EAsyncExecution::Thread:
            {
                char ThreadName[64];
                snprintf(ThreadName, sizeof(ThreadName), "TAsync %d", Private::FAsyncThreadIndex::GetNext());
                Private::LaunchAsyncThread(MoveTemp(Function), MoveTemp(Promise),
                                           MoveTemp(CompletionCallback), ThreadName,
                                           0, EThreadPriority::TPri_Normal);
            }
            break;

            case EAsyncExecution::ThreadPool:
                // ThreadPool requires FQueuedThreadPool - use AsyncPool() directly
                // Fall back to TaskGraph for now
                {
                    Private::LaunchFireAndForget(
                        "AsyncPoolTask",
                        LowLevelTasks::ETaskPriority::BackgroundNormal,
                        [Function = MoveTemp(Function), Promise = MoveTemp(Promise),
                         Callback = MoveTemp(CompletionCallback)]() mutable
                        {
                            SetPromise(Promise, Function);
                            if (Callback)
                            {
                                Callback();
                            }
                        });
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
     *        Runs after the result on the dedicated thread, before callable
     *        captures are destroyed; startup fallback runs inline.
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

        char ThreadName[64];
        snprintf(ThreadName, sizeof(ThreadName), "TAsyncThread %d", Private::FAsyncThreadIndex::GetNext());
        Private::LaunchAsyncThread(MoveTemp(Function), MoveTemp(Promise),
                                   MoveTemp(CompletionCallback), ThreadName, StackSize, ThreadPri);

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
        Private::LaunchFireAndForget("AsyncTask", Priority, MoveTemp(Function));
    }

    /**
     * @brief Execute a task with default priority
     */
    inline void AsyncTask(TUniqueFunction<void()> Function)
    {
        AsyncTask(LowLevelTasks::ETaskPriority::Normal, MoveTemp(Function));
    }

} // namespace OloEngine
