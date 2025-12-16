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
            : m_Function(MoveTemp(InFunction))
            , m_Promise(MoveTemp(InPromise))
        {
        }

        virtual u32 Run() override
        {
            SetPromise(m_Promise, m_Function);

            // Mark as complete so the thread can be cleaned up
            m_bComplete.store(true, std::memory_order_release);
            
            return 0;
        }

        virtual void Exit() override
        {
            // Schedule self-destruction - the thread will clean up after itself
            // We need to delete the runnable after the thread is done
        }

        bool IsComplete() const
        {
            return m_bComplete.load(std::memory_order_acquire);
        }

        void SetThread(FRunnableThread* InThread)
        {
            m_Thread = InThread;
        }

        FRunnableThread* GetThread() const { return m_Thread; }

    private:
        TUniqueFunction<ResultType()> m_Function;
        TPromise<ResultType> m_Promise;
        FRunnableThread* m_Thread = nullptr;
        std::atomic<bool> m_bComplete{false};
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
                static std::atomic<i32> s_Counter{0};
                return s_Counter.fetch_add(1, std::memory_order_relaxed);
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
        
        if (CompletionCallback)
        {
            // Wrap the completion callback
            auto State = Promise.GetFuture();
            // Note: We need to set continuation before getting the future
        }
        
        TFuture<ResultType> Future = Promise.GetFuture();

        switch (Execution)
        {
        case EAsyncExecution::TaskGraph:
            {
                // Launch on the task scheduler
                LowLevelTasks::FTask* Task = new LowLevelTasks::FTask();
                Task->Init(
                    "AsyncTask",
                    LowLevelTasks::ETaskPriority::Normal,
                    [Function = MoveTemp(Function), Promise = MoveTemp(Promise), 
                     Callback = MoveTemp(CompletionCallback), Task](bool bNotCanceled) mutable -> LowLevelTasks::FTask*
                    {
                        if (bNotCanceled)
                        {
                            SetPromise(Promise, Function);
                        }
                        if (Callback)
                        {
                            Callback();
                        }
                        delete Task;
                        return nullptr;
                    },
                    LowLevelTasks::ETaskFlags::DefaultFlags
                );
                LowLevelTasks::TryLaunch(*Task);
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
                    0,  // Default stack size
                    EThreadPriority::TPri_Normal
                );

                if (Thread)
                {
                    Runnable->SetThread(Thread);
                    
                    // Schedule cleanup on task graph after completion
                    // The thread will self-clean via the runnable's Exit()
                    LowLevelTasks::FTask* CleanupTask = new LowLevelTasks::FTask();
                    CleanupTask->Init(
                        "AsyncCleanup",
                        LowLevelTasks::ETaskPriority::BackgroundLow,
                        [Runnable, Thread, Callback = MoveTemp(CompletionCallback), CleanupTask](bool) mutable -> LowLevelTasks::FTask*
                        {
                            // Wait for completion and clean up
                            Thread->WaitForCompletion();
                            delete Thread;
                            delete Runnable;
                            if (Callback)
                            {
                                Callback();
                            }
                            delete CleanupTask;
                            return nullptr;
                        },
                        LowLevelTasks::ETaskFlags::DefaultFlags
                    );
                    LowLevelTasks::TryLaunch(*CleanupTask);
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
                LowLevelTasks::FTask* Task = new LowLevelTasks::FTask();
                Task->Init(
                    "AsyncPoolTask",
                    LowLevelTasks::ETaskPriority::BackgroundNormal,
                    [Function = MoveTemp(Function), Promise = MoveTemp(Promise), 
                     Callback = MoveTemp(CompletionCallback), Task](bool bNotCanceled) mutable -> LowLevelTasks::FTask*
                    {
                        if (bNotCanceled)
                        {
                            SetPromise(Promise, Function);
                        }
                        if (Callback)
                        {
                            Callback();
                        }
                        delete Task;
                        return nullptr;
                    },
                    LowLevelTasks::ETaskFlags::DefaultFlags
                );
                LowLevelTasks::TryLaunch(*Task);
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
                ThreadPri
            );

            if (Thread)
            {
                Runnable->SetThread(Thread);
                
                // Schedule cleanup
                LowLevelTasks::FTask* CleanupTask = new LowLevelTasks::FTask();
                CleanupTask->Init(
                    "AsyncThreadCleanup",
                    LowLevelTasks::ETaskPriority::BackgroundLow,
                    [Runnable, Thread, Callback = MoveTemp(CompletionCallback), CleanupTask](bool) mutable -> LowLevelTasks::FTask*
                    {
                        Thread->WaitForCompletion();
                        delete Thread;
                        delete Runnable;
                        if (Callback)
                        {
                            Callback();
                        }
                        delete CleanupTask;
                        return nullptr;
                    },
                    LowLevelTasks::ETaskFlags::DefaultFlags
                );
                LowLevelTasks::TryLaunch(*CleanupTask);
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
        LowLevelTasks::FTask* Task = new LowLevelTasks::FTask();
        Task->Init(
            "AsyncTask",
            Priority,
            [Function = MoveTemp(Function), Task](bool bNotCanceled) mutable -> LowLevelTasks::FTask*
            {
                if (bNotCanceled)
                {
                    Function();
                }
                delete Task;
                return nullptr;
            },
            LowLevelTasks::ETaskFlags::DefaultFlags
        );
        LowLevelTasks::TryLaunch(*Task);
    }

    /**
     * @brief Execute a task with default priority
     */
    inline void AsyncTask(TUniqueFunction<void()> Function)
    {
        AsyncTask(LowLevelTasks::ETaskPriority::Normal, MoveTemp(Function));
    }

} // namespace OloEngine
