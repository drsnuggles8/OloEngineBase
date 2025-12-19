// TaskTag.h - Named thread identification and tagging system
// Ported from UE5.7 ETaskTag and FTaskTagScope

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Misc/EnumClassFlags.h"
#include <atomic>

namespace OLO
{
    /**
     * Task tags used to identify execution contexts (threads or jobs).
     * Used for IsInGameThread(), IsInRenderingThread(), etc.
     */
    enum class ETaskTag : i32
    {
        ENone = 0 << 0,
        EStaticInit = 1 << 0,         // During static initialization (before main())
        EGameThread = 1 << 1,         // Main game thread
        ESlateThread = 1 << 2,        // Slate loading thread
        ERenderingThread = 1 << 4,    // Rendering thread
        ERhiThread = 1 << 5,          // RHI thread
        EAsyncLoadingThread = 1 << 6, // Async loading thread
        EEventThread = 1 << 7,        // Event processing thread

        // Mask for all named thread bits
        ENamedThreadBits = (EEventThread << 1) - 1,

        // Can be used when multiple threads/jobs are involved (parallel for)
        // Avoids uniqueness check for the tag
        EParallelThread = 1 << 30,

        EWorkerThread = 1 << 29 | EParallelThread,
        EParallelRenderingThread = ERenderingThread | EParallelThread,
        EParallelGameThread = EGameThread | EParallelThread,
        EParallelRhiThread = ERhiThread | EParallelThread,
        EParallelLoadingThread = EAsyncLoadingThread | EParallelThread,
    };

    ENUM_CLASS_FLAGS(ETaskTag)

    /**
     * RAII scope for tagging an execution context (thread or job).
     * Allows querying the current thread type via IsInGameThread(), etc.
     *
     * Usage:
     *   {
     *       FTaskTagScope Scope(ETaskTag::EGameThread);
     *       // This thread is now tagged as the game thread
     *   }
     *   // Tag restored to previous value
     */
    class FTaskTagScope
    {
        friend class FRunnableThread;

      public:
        /**
         * Constructs a scope that tags the current execution context.
         * @param InTag The tag to apply (must not be ENone or EParallelThread alone)
         */
        FTaskTagScope(ETaskTag InTag);
        ~FTaskTagScope();

        // Non-copyable
        FTaskTagScope(const FTaskTagScope&) = delete;
        FTaskTagScope& operator=(const FTaskTagScope&) = delete;

        /**
         * Gets the currently active task tag for this thread.
         */
        static ETaskTag GetCurrentTag();

        /**
         * Checks if the current tag matches the specified tag.
         */
        static bool IsCurrentTag(ETaskTag InTag);

        /**
         * Checks if we're running during static initialization.
         */
        static bool IsRunningDuringStaticInit();

        /**
         * Clears the EStaticInit tag so functions like IsInGameThread() work properly.
         * Called at start of main() or equivalent.
         */
        static void SetTagNone();

        /**
         * Restores the EStaticInit tag for proper handling during static destruction.
         */
        static void SetTagStaticInit();

        /**
         * Swaps the current tag and returns the old one.
         * Used when thread contexts move between different threads.
         */
        static ETaskTag SwapTag(ETaskTag NewTag);

      private:
        static i32 GetStaticThreadId();

      private:
        static thread_local ETaskTag s_ActiveTaskTag;
        static std::atomic<i32> s_ActiveNamedThreads;

        ETaskTag ParentTag; // Tag before this scope
        ETaskTag Tag;       // Tag for this scope
        bool TagOnlyIfNone; // Only apply tag if current tag is None
    };

    //////////////////////////////////////////////////////////////////////////
    // Global thread ID tracking

    /** Thread ID of the main/game thread */
    extern u32 GGameThreadId;

    /** Thread ID of the render thread (if any) */
    extern u32 GRenderThreadId;

    /** Thread ID of the slate loading thread (if any) */
    extern u32 GSlateLoadingThreadId;

    /** Thread ID of the RHI thread (if any) */
    extern u32 GRHIThreadId;

    /** Has GGameThreadId been set yet? */
    extern bool GIsGameThreadIdInitialized;

    /** Is the RHI running in a separate thread? */
    extern bool GIsRunningRHIInSeparateThread;

    /** Is the RHI running in a dedicated thread? */
    extern bool GIsRunningRHIInDedicatedThread;

    //////////////////////////////////////////////////////////////////////////
    // Thread query functions

    /**
     * Returns true if the current thread is the game thread.
     */
    bool IsInGameThread();

    /**
     * Returns true if the current thread is in a parallel game thread context.
     */
    bool IsInParallelGameThread();

    /**
     * Returns true if the current thread is the actual rendering thread.
     */
    bool IsInActualRenderingThread();

    /**
     * Returns true if the current thread is the rendering thread or game thread (when no separate render thread).
     */
    bool IsInRenderingThread();

    /**
     * Returns true if the current thread is in any rendering context (parallel or not).
     */
    bool IsInAnyRenderingThread();

    /**
     * Returns true if the current thread is in a parallel rendering context.
     */
    bool IsInParallelRenderingThread();

    /**
     * Returns true if the RHI thread is currently running.
     */
    bool IsRHIThreadRunning();

    /**
     * Returns true if the current thread is the RHI thread.
     */
    bool IsInRHIThread();

    /**
     * Returns true if the current thread is in a parallel RHI context.
     */
    bool IsInParallelRHIThread();

    /**
     * Returns true if the current thread is the slate loading thread.
     */
    bool IsInSlateThread();

    /**
     * Returns true if the current thread is a worker thread.
     */
    bool IsInWorkerThread();

    /**
     * Returns true if the current thread is the async loading thread (and not the game thread).
     */
    bool IsInActualLoadingThread();

    /**
     * Returns true if the current thread is in a parallel loading context.
     */
    bool IsInParallelLoadingThread();

} // namespace OLO
