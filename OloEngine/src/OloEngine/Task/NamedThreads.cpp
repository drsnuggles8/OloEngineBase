// @file NamedThreads.cpp
// @brief Named thread dispatch system implementation
//
// This file provides the definitions for thread-local storage and any
// non-inline implementations for the named thread system.
//
// @see NamedThreads.h for architecture notes and detailed documentation

#include "OloEngine/Task/NamedThreads.h"

namespace OloEngine::Tasks
{
    // ============================================================================
    // Thread-local storage definitions
    // ============================================================================

    // @brief Thread-local storage for current named thread identity
    //
    // Each thread that calls AttachToThread() gets this set to their role.
    // Worker threads and other threads have this set to ENamedThread::Invalid.
    thread_local ENamedThread FNamedThreadManager::s_CurrentNamedThread = ENamedThread::Invalid;

    // @brief Thread-local flag tracking if we're currently processing tasks
    //
    // Used to prevent re-entrancy in TryWaitOnNamedThread.
    thread_local bool FNamedThreadManager::s_bIsProcessingTasks = false;

    // ============================================================================
    // Configuration globals
    // ============================================================================

    // @brief Global configuration for named thread wait behavior
    //
    // When true, waiting on any task will automatically process named thread
    // tasks if the current thread is a named thread. This helps prevent deadlocks
    // where a named thread waits on a task that might schedule work back to
    // that same thread.
    //
    // This mirrors UE5.7's GTaskGraphAlwaysWaitWithNamedThreadSupport.
    //
    // Default is false for backwards compatibility, but production code should
    // consider enabling this to avoid subtle deadlock scenarios.
    bool GTaskGraphAlwaysWaitWithNamedThreadSupport = false;

    // @brief Check if extended priority should force named thread wait support
    //
    // Some priority types inherently require named thread processing during waits.
    // This function identifies those cases.
    //
    // @param Priority The extended priority to check
    // @return true if this priority should always use named thread wait support
    bool ShouldForceWaitWithNamedThreadsSupport(EExtendedTaskPriority Priority)
    {
        // Named thread priorities should always use named thread wait support
        // to avoid deadlocks when waiting for tasks that target the same thread
        return IsNamedThreadPriority(Priority);
    }

} // namespace OloEngine::Tasks
