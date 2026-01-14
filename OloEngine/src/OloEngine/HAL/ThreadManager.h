// ThreadManager.h - Global thread registry
// Ported from UE5.7 FThreadManager

#pragma once

/**
 * @file ThreadManager.h
 * @brief Manages runnables and runnable threads registry
 *
 * Provides a global registry of all FRunnableThread instances,
 * supporting thread enumeration, name lookup, and debugging facilities.
 *
 * Ported from Unreal Engine 5.7
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/CriticalSection.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace OloEngine
{
    class FRunnableThread;
    class FForkProcessHelper;

    /**
     * @class FThreadManager
     * @brief Manages runnables and runnable threads
     *
     * Provides a central registry for all threads created via FRunnableThread,
     * enabling enumeration, debugging, and coordination during fork operations.
     */
    class FThreadManager
    {
      public:
        FThreadManager();
        ~FThreadManager();

        // Non-copyable
        FThreadManager(const FThreadManager&) = delete;
        FThreadManager& operator=(const FThreadManager&) = delete;

        /**
         * @brief Used internally to add a new thread object
         * @param ThreadId The thread's ID
         * @param Thread Pointer to the thread object
         * @see RemoveThread
         */
        void AddThread(u32 ThreadId, FRunnableThread* Thread);

        /**
         * @brief Used internally to remove thread object
         * @param Thread Thread object to be removed
         * @see AddThread
         */
        void RemoveThread(FRunnableThread* Thread);

        /**
         * @brief Get the number of registered threads
         */
        i32 NumThreads() const
        {
            return static_cast<i32>(m_Threads.size());
        }

        /**
         * @brief Ticks all fake threads and their runnable objects
         *
         * Fake threads are pseudo-threads used in single-threaded mode
         * that must be manually ticked on the main thread.
         */
        void Tick();

        /**
         * @brief Returns the name of a thread given its TLS id
         * @param ThreadId The thread ID to look up
         * @return Reference to the thread name string
         */
        static const std::string& GetThreadName(u32 ThreadId);

        /**
         * @brief Enumerate each thread
         * @param Func Callback invoked for each registered thread
         */
        void ForEachThread(std::function<void(u32 ThreadId, FRunnableThread* Thread)> Func);

#if defined(_WIN32) || defined(__APPLE__)
#define OLO_SUPPORTS_ALL_THREAD_BACKTRACES 1
#else
#define OLO_SUPPORTS_ALL_THREAD_BACKTRACES 0
#endif

#if OLO_SUPPORTS_ALL_THREAD_BACKTRACES
        /**
         * @struct FThreadStackBackTrace
         * @brief Holds stack backtrace information for a thread
         */
        struct FThreadStackBackTrace
        {
            static constexpr u32 ProgramCountersMaxStackSize = 100;

            u32 ThreadId;
            std::string ThreadName;
            std::vector<u64> ProgramCounters;

            FThreadStackBackTrace() : ThreadId(0)
            {
                ProgramCounters.reserve(ProgramCountersMaxStackSize);
            }
        };

        /**
         * @brief Get stack backtraces for all registered threads
         * @param OutStackTraces Vector to receive the stack traces
         */
        void GetAllThreadStackBackTraces(std::vector<FThreadStackBackTrace>& OutStackTraces);

        /**
         * @brief Enumerate through all thread stack backtraces
         * @param Func Callback for each thread's stack trace. Return true to continue, false to stop.
         *
         * This function is primarily intended to iterate over stack traces in a crashing context
         * and avoids allocation of additional memory. It does not perform safety checks to ensure
         * that the list of threads is not modified mid-iteration.
         *
         * The thread name and stack trace array memory are only valid for the duration of the
         * callback's execution and must be copied elsewhere if needed beyond its scope.
         */
        void ForEachThreadStackBackTrace(std::function<bool(u32 ThreadId, const char* ThreadName, const std::vector<u64>& StackTrace)> Func);
#endif

        /**
         * @brief Access to the singleton object
         * @return Reference to the thread manager
         */
        static FThreadManager& Get();

      private:
        friend class FForkProcessHelper;

        /**
         * @brief Returns a list of registered forkable threads
         */
        std::vector<FRunnableThread*> GetForkableThreads();

        /**
         * @brief Returns internal name of a thread given its TLS id
         */
        const std::string& GetThreadNameInternal(u32 ThreadId);

        /**
         * @brief Notification that the parent is about to fork
         */
        void HandleOnParentPreFork();

        /**
         * @brief Check if it's safe to continue thread list iteration
         */
        bool CheckThreadListSafeToContinueIteration();

        /**
         * @brief Called when the thread list is modified
         */
        void OnThreadListModified();

      private:
        /** Critical section for ThreadList */
        mutable FCriticalSection m_ThreadsCritical;

        /** Map of thread ID to thread object */
        std::unordered_map<u32, FRunnableThread*> m_Threads;

        /** Helper variable for catching unexpected modification of the thread map/list */
        bool m_bIsThreadListDirty = false;

        /** Static empty string for unknown threads */
        inline static std::string s_UnknownThreadName = "UnknownThread";
    };

} // namespace OloEngine

//=============================================================================
// IMPLEMENTATION
//=============================================================================

#include "OloEngine/HAL/RunnableThread.h"

namespace OloEngine
{
    inline FThreadManager::FThreadManager() = default;

    inline FThreadManager::~FThreadManager() = default;

    inline FThreadManager& FThreadManager::Get()
    {
        static FThreadManager s_Instance;
        return s_Instance;
    }

    inline void FThreadManager::AddThread(u32 ThreadId, FRunnableThread* Thread)
    {
        FScopeLock Lock(&m_ThreadsCritical);
        m_Threads[ThreadId] = Thread;
        OnThreadListModified();
    }

    inline void FThreadManager::RemoveThread(FRunnableThread* Thread)
    {
        FScopeLock Lock(&m_ThreadsCritical);
        for (auto It = m_Threads.begin(); It != m_Threads.end(); ++It)
        {
            if (It->second == Thread)
            {
                m_Threads.erase(It);
                OnThreadListModified();
                break;
            }
        }
    }

    inline void FThreadManager::Tick()
    {
        FScopeLock Lock(&m_ThreadsCritical);

        // Reset dirty flag before iteration
        m_bIsThreadListDirty = false;

        for (auto& [ThreadId, Thread] : m_Threads)
        {
            if (!CheckThreadListSafeToContinueIteration())
            {
                break;
            }

            // Tick fake threads only
            if (Thread && Thread->GetThreadType() == FRunnableThread::ThreadType::Fake)
            {
                // Fake thread tick would be implemented here
                // For now, this is a placeholder
            }
        }
    }

    inline const std::string& FThreadManager::GetThreadName(u32 ThreadId)
    {
        // Look up from the thread registry
        return Get().GetThreadNameInternal(ThreadId);
    }

    inline const std::string& FThreadManager::GetThreadNameInternal(u32 ThreadId)
    {
        FScopeLock Lock(&m_ThreadsCritical);
        auto It = m_Threads.find(ThreadId);
        if (It != m_Threads.end() && It->second)
        {
            return It->second->GetThreadName();
        }
        return s_UnknownThreadName;
    }

    inline void FThreadManager::ForEachThread(std::function<void(u32 ThreadId, FRunnableThread* Thread)> Func)
    {
        FScopeLock Lock(&m_ThreadsCritical);

        m_bIsThreadListDirty = false;

        for (auto& [ThreadId, Thread] : m_Threads)
        {
            if (!CheckThreadListSafeToContinueIteration())
            {
                break;
            }
            Func(ThreadId, Thread);
        }
    }

    inline std::vector<FRunnableThread*> FThreadManager::GetForkableThreads()
    {
        std::vector<FRunnableThread*> ForkableThreads;

        FScopeLock Lock(&m_ThreadsCritical);
        for (auto& [ThreadId, Thread] : m_Threads)
        {
            if (Thread && Thread->GetThreadType() == FRunnableThread::ThreadType::Forkable)
            {
                ForkableThreads.push_back(Thread);
            }
        }

        return ForkableThreads;
    }

    inline void FThreadManager::HandleOnParentPreFork()
    {
        // Notify all forkable threads that a fork is about to happen
        auto ForkableThreads = GetForkableThreads();
        for (FRunnableThread* Thread : ForkableThreads)
        {
            // Would notify each forkable thread here
            (void)Thread;
        }
    }

    inline bool FThreadManager::CheckThreadListSafeToContinueIteration()
    {
        return !m_bIsThreadListDirty;
    }

    inline void FThreadManager::OnThreadListModified()
    {
        m_bIsThreadListDirty = true;
    }

#if OLO_SUPPORTS_ALL_THREAD_BACKTRACES
    inline void FThreadManager::GetAllThreadStackBackTraces(std::vector<FThreadStackBackTrace>& OutStackTraces)
    {
        FScopeLock Lock(&m_ThreadsCritical);

        OutStackTraces.clear();
        OutStackTraces.reserve(m_Threads.size());

        for (auto& [ThreadId, Thread] : m_Threads)
        {
            if (Thread)
            {
                FThreadStackBackTrace StackTrace;
                StackTrace.ThreadId = ThreadId;
                StackTrace.ThreadName = Thread->GetThreadName();

                // Platform-specific stack walking would go here
                // For now, we leave ProgramCounters empty

                OutStackTraces.push_back(std::move(StackTrace));
            }
        }
    }

    inline void FThreadManager::ForEachThreadStackBackTrace(
        std::function<bool(u32 ThreadId, const char* ThreadName, const std::vector<u64>& StackTrace)> Func)
    {
        // Note: This function is designed for crash contexts and avoids allocations
        // It does NOT lock the critical section to avoid potential deadlocks during crashes

        for (auto& [ThreadId, Thread] : m_Threads)
        {
            if (Thread)
            {
                std::vector<u64> StackTrace;
                // Platform-specific stack walking would go here

                if (!Func(ThreadId, Thread->GetThreadName().c_str(), StackTrace))
                {
                    break;
                }
            }
        }
    }
#endif

} // namespace OloEngine
