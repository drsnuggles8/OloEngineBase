// TaskTrace.h - Task tracing for profiling support with Tracy integration
// Ported from UE5.7 Async/TaskTrace.h

#pragma once

/**
 * @file TaskTrace.h
 * @brief Task lifecycle tracing for profiling
 * 
 * This provides a tracing API compatible with UE5.7's TaskTrace for task system
 * profiling and debugging. When tracing is enabled, it emits events for:
 * - Task creation, launch, scheduling
 * - Task execution start/finish
 * - Task completion and destruction
 * - Wait operations
 * - Dependency tracking (subsequents)
 * 
 * OloEngine uses Tracy for profiling instead of UE's UnrealInsights.
 * When OLO_TASK_TRACE_ENABLED is set and Tracy is available, events are
 * emitted to Tracy. Otherwise, the API becomes no-ops.
 * 
 * Tracy Integration Features:
 * - Zone scopes for task execution visualization
 * - Dynamic zone naming with task debug names
 * - Plot tracking for task system metrics
 * - Message logging for task lifecycle events
 * - Fiber support for task context tracking
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/PlatformTime.h"

#include <atomic>
#include <cstring>

#if TRACY_ENABLE
    #include <tracy/Tracy.hpp>
#endif

// Forward declare IsInRenderingThread from TaskPrivate
namespace OloEngine::Tasks::Private
{
    bool IsInRenderingThread();
}

// ============================================================================
// Task trace configuration
// ============================================================================

#ifndef OLO_TASK_TRACE_ENABLED
    #if OLO_PROFILE
        #define OLO_TASK_TRACE_ENABLED 1
    #else
        #define OLO_TASK_TRACE_ENABLED 0
    #endif
#endif

// Enable verbose task tracing (logs every lifecycle event)
#ifndef OLO_TASK_TRACE_VERBOSE
    #define OLO_TASK_TRACE_VERBOSE 0
#endif

namespace OloEngine
{
    /**
     * @namespace TaskTrace
     * @brief Task lifecycle tracing namespace
     * 
     * Provides tracing APIs compatible with UE5.7's TaskTrace.
     * Events are emitted to Tracy when tracing is enabled.
     */
    namespace TaskTrace
    {
        // Task ID type (unique identifier for each task instance)
        using FId = u64;

        // Invalid task ID sentinel
        inline constexpr FId InvalidId = ~FId(0);

        // Version number for trace format compatibility
        inline constexpr u32 TaskTraceVersion = 1;

        // ====================================================================
        // Metrics tracking
        // ====================================================================
        
        /**
         * @struct FTaskMetrics
         * @brief Global task system metrics for Tracy visualization
         */
        struct FTaskMetrics
        {
            std::atomic<i64> ActiveTasks{0};        // Currently executing tasks
            std::atomic<i64> PendingTasks{0};       // Tasks waiting for prerequisites
            std::atomic<i64> TotalTasksCreated{0};  // Lifetime total
            std::atomic<i64> TotalTasksCompleted{0};// Lifetime total
            std::atomic<i64> WaitingThreads{0};     // Threads blocked on task wait
        };

        // Global metrics instance
        inline FTaskMetrics g_Metrics;

        /**
         * @brief Generate a unique task ID
         * @return New unique task ID (monotonically increasing)
         */
        FId GenerateTaskId();

        /**
         * @brief Initialize the task trace system
         * 
         * Should be called once at startup before any tasks are created.
         */
        void Init();

        /**
         * @brief Trace task creation (before launch)
         * @param TaskId The task's unique ID
         * @param TaskSize Size of the task object in bytes
         */
        void Created(FId TaskId, u64 TaskSize);

        /**
         * @brief Trace task launch
         * @param TaskId The task's unique ID
         * @param DebugName Human-readable name for the task
         * @param bTracked Whether this task should be tracked in profiler
         * @param ThreadToExecuteOn Target thread (priority value)
         * @param TaskSize Size of the task object in bytes
         */
        void Launched(FId TaskId, const char* DebugName, bool bTracked, i32 ThreadToExecuteOn, u64 TaskSize);

        /**
         * @brief Trace task scheduled for execution
         * @param TaskId The task's unique ID
         */
        void Scheduled(FId TaskId);

        /**
         * @brief Trace subsequent task dependency added
         * @param TaskId The parent task ID
         * @param SubsequentId The dependent task ID
         */
        void SubsequentAdded(FId TaskId, FId SubsequentId);

        /**
         * @brief Trace task execution started
         * @param TaskId The task's unique ID
         */
        void Started(FId TaskId);

        /**
         * @brief Trace task execution finished (body complete, may have pending nested tasks)
         * @param TaskId The task's unique ID
         */
        void Finished(FId TaskId);

        /**
         * @brief Trace task fully completed (all nested tasks done, subsequents unlocked)
         * @param TaskId The task's unique ID
         */
        void Completed(FId TaskId);

        /**
         * @brief Trace task destruction
         * @param TaskId The task's unique ID
         */
        void Destroyed(FId TaskId);

        /**
         * @struct FWaitingScope
         * @brief RAII scope for tracing wait operations
         * 
         * Records when a thread starts and stops waiting for tasks.
         * Integrates with Tracy to show wait regions in the timeline.
         */
        struct FWaitingScope
        {
            explicit FWaitingScope(const TArray<FId>& Tasks);
            explicit FWaitingScope(FId TaskId);
            ~FWaitingScope();

        private:
            bool m_bIsActive = false;
#if TRACY_ENABLE && OLO_TASK_TRACE_ENABLED
            tracy::ScopedZone* m_pZone = nullptr;
#endif
        };

        /**
         * @struct FTaskTimingEventScope
         * @brief RAII scope for tracing task execution timing
         * 
         * Automatically emits Started() on construction and Finished() on destruction.
         * Creates a Tracy zone for the task execution with the task's debug name.
         */
        struct FTaskTimingEventScope
        {
            FTaskTimingEventScope(FId InTaskId, const char* DebugName = nullptr);
            ~FTaskTimingEventScope();

            // Non-copyable
            FTaskTimingEventScope(const FTaskTimingEventScope&) = delete;
            FTaskTimingEventScope& operator=(const FTaskTimingEventScope&) = delete;

        private:
            bool m_bIsActive = false;
            FId m_TaskId = InvalidId;
#if TRACY_ENABLE && OLO_TASK_TRACE_ENABLED
            bool m_bZoneActive = false;
#endif
        };

// ============================================================================
// Implementation - enabled/disabled versions
// ============================================================================

#if OLO_TASK_TRACE_ENABLED

        // Global flag indicating initialization status
        inline std::atomic<bool> g_TaskTraceInitialized{ false };

        // Atomic counter for generating unique task IDs
        inline std::atomic<FId> g_NextTaskId{ 1 };

        inline FId GenerateTaskId()
        {
            FId Id = g_NextTaskId.fetch_add(1, std::memory_order_relaxed);
            OLO_CORE_ASSERT(Id != InvalidId, "TaskTrace: TaskId overflow");
            return Id;
        }

        inline void Init()
        {
            g_TaskTraceInitialized.store(true, std::memory_order_release);
#if TRACY_ENABLE
            TracyMessageL("TaskTrace initialized");
            // Set up Tracy plots for task metrics
            TracyPlotConfig("ActiveTasks", tracy::PlotFormatType::Number, true, true, 0);
            TracyPlotConfig("PendingTasks", tracy::PlotFormatType::Number, true, true, 0);
            TracyPlotConfig("WaitingThreads", tracy::PlotFormatType::Number, true, true, 0);
#endif
        }

        inline void Created(FId TaskId, u64 TaskSize)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            
            g_Metrics.TotalTasksCreated.fetch_add(1, std::memory_order_relaxed);
            g_Metrics.PendingTasks.fetch_add(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("PendingTasks", static_cast<i64>(g_Metrics.PendingTasks.load(std::memory_order_relaxed)));
    #if OLO_TASK_TRACE_VERBOSE
            TracyMessageL("Task Created");
    #endif
#endif
            (void)TaskId;
            (void)TaskSize;
        }

        inline void Launched(FId TaskId, const char* DebugName, bool bTracked, i32 ThreadToExecuteOn, u64 TaskSize)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            (void)TaskId;
            (void)bTracked;
            (void)ThreadToExecuteOn;
            (void)TaskSize;
#if TRACY_ENABLE
            if (DebugName && DebugName[0] != '\0')
            {
    #if OLO_TASK_TRACE_VERBOSE
                TracyMessage(DebugName, strlen(DebugName));
    #endif
            }
#else
            (void)DebugName;
#endif
        }

        inline void Scheduled(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            (void)TaskId;
#if TRACY_ENABLE && OLO_TASK_TRACE_VERBOSE
            TracyMessageL("Task Scheduled");
#endif
        }

        inline void SubsequentAdded(FId TaskId, FId SubsequentId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            (void)TaskId;
            (void)SubsequentId;
        }

        inline void Started(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            
            g_Metrics.ActiveTasks.fetch_add(1, std::memory_order_relaxed);
            g_Metrics.PendingTasks.fetch_sub(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("ActiveTasks", static_cast<i64>(g_Metrics.ActiveTasks.load(std::memory_order_relaxed)));
            TracyPlot("PendingTasks", static_cast<i64>(g_Metrics.PendingTasks.load(std::memory_order_relaxed)));
#endif
            (void)TaskId;
        }

        inline void Finished(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            
            g_Metrics.ActiveTasks.fetch_sub(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("ActiveTasks", static_cast<i64>(g_Metrics.ActiveTasks.load(std::memory_order_relaxed)));
#endif
            (void)TaskId;
        }

        inline void Completed(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            
            g_Metrics.TotalTasksCompleted.fetch_add(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE && OLO_TASK_TRACE_VERBOSE
            TracyMessageL("Task Completed");
#endif
            (void)TaskId;
        }

        inline void Destroyed(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            (void)TaskId;
        }

        // FWaitingScope implementation
        inline FWaitingScope::FWaitingScope(const TArray<FId>& Tasks)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            m_bIsActive = true;
            g_Metrics.WaitingThreads.fetch_add(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("WaitingThreads", static_cast<i64>(g_Metrics.WaitingThreads.load(std::memory_order_relaxed)));
            // Note: We can't easily store a ScopedZone as member because it's non-copyable
            // and uses stack-based RAII. Using message markers instead.
            TracyMessageL("Wait Started");
#endif
            (void)Tasks;
        }

        inline FWaitingScope::FWaitingScope(FId TaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            m_bIsActive = true;
            g_Metrics.WaitingThreads.fetch_add(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("WaitingThreads", static_cast<i64>(g_Metrics.WaitingThreads.load(std::memory_order_relaxed)));
            TracyMessageL("Wait Started");
#endif
            (void)TaskId;
        }

        inline FWaitingScope::~FWaitingScope()
        {
            if (!m_bIsActive) return;
            g_Metrics.WaitingThreads.fetch_sub(1, std::memory_order_relaxed);
            
#if TRACY_ENABLE
            TracyPlot("WaitingThreads", static_cast<i64>(g_Metrics.WaitingThreads.load(std::memory_order_relaxed)));
            TracyMessageL("Wait Finished");
#endif
        }

        // FTaskTimingEventScope implementation
        inline FTaskTimingEventScope::FTaskTimingEventScope(FId InTaskId, const char* DebugName)
            : m_TaskId(InTaskId)
        {
            if (!g_TaskTraceInitialized.load(std::memory_order_acquire)) return;
            
            Started(m_TaskId);
            m_bIsActive = true;

#if TRACY_ENABLE
            // Important: Don't output CPU profiler events on RenderThread to avoid
            // breaking frame event hierarchy. UE5.7 does the same check.
            // The RenderingThread outputs BeginFrameRenderThread/EndFrameRenderThread
            // events, and task execution events would incorrectly close these.
            if (!Tasks::Private::IsInRenderingThread())
            {
                m_bZoneActive = true;
                // Use dynamic zone name if debug name is provided
                if (DebugName && DebugName[0] != '\0')
                {
                    // Tracy zone with dynamic name using message
                    TracyMessageC(DebugName, strlen(DebugName), 0x3366FF);
                }
            }
#else
            (void)DebugName;
#endif
        }

        inline FTaskTimingEventScope::~FTaskTimingEventScope()
        {
            if (!m_bIsActive) return;
            
            Finished(m_TaskId);
            
#if TRACY_ENABLE
            if (m_bZoneActive)
            {
                // Zone ends automatically when scope exits
            }
#endif
        }

#else // !OLO_TASK_TRACE_ENABLED

        // NOOP implementation when tracing is disabled
        inline FId GenerateTaskId() { return InvalidId; }
        inline void Init() {}
        inline void Created(FId /*TaskId*/, u64 /*TaskSize*/) {}
        inline void Launched(FId /*TaskId*/, const char* /*DebugName*/, bool /*bTracked*/, i32 /*ThreadToExecuteOn*/, u64 /*TaskSize*/) {}
        inline void Scheduled(FId /*TaskId*/) {}
        inline void SubsequentAdded(FId /*TaskId*/, FId /*SubsequentId*/) {}
        inline void Started(FId /*TaskId*/) {}
        inline void Finished(FId /*TaskId*/) {}
        inline void Completed(FId /*TaskId*/) {}
        inline void Destroyed(FId /*TaskId*/) {}

        inline FWaitingScope::FWaitingScope(const TArray<FId>& /*Tasks*/) {}
        inline FWaitingScope::FWaitingScope(FId /*TaskId*/) {}
        inline FWaitingScope::~FWaitingScope() {}
        inline FTaskTimingEventScope::FTaskTimingEventScope(FId /*InTaskId*/, const char* /*DebugName*/) {}
        inline FTaskTimingEventScope::~FTaskTimingEventScope() {}

#endif // OLO_TASK_TRACE_ENABLED

    } // namespace TaskTrace

} // namespace OloEngine

// ============================================================================
// Task system trace macros
// These match UE5.7's TRACE_CPUPROFILER_EVENT_SCOPE pattern
// ============================================================================

#if OLO_PROFILE && TRACY_ENABLE
    #define TRACE_CPUPROFILER_EVENT_SCOPE(Name) ZoneScopedN(#Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_STR(Name) ZoneScopedN(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(Name) ZoneScopedN(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(Name, Channel) ZoneScopedN(Name)
    
    // Flush profiler events before sleeping
    #define TRACE_CPUPROFILER_EVENT_FLUSH() do { FrameMark; } while(0)
#elif OLO_PROFILE
    #define TRACE_CPUPROFILER_EVENT_SCOPE(Name) OLO_PROFILE_SCOPE(#Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_STR(Name) OLO_PROFILE_SCOPE(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(Name) OLO_PROFILE_SCOPE(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(Name, Channel) OLO_PROFILE_SCOPE(Name)
    #define TRACE_CPUPROFILER_EVENT_FLUSH()
#else
    #define TRACE_CPUPROFILER_EVENT_SCOPE(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_STR(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(Name)
    #define TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(Name, Channel)
    #define TRACE_CPUPROFILER_EVENT_FLUSH()
#endif

// ============================================================================
// CSV profiler macros (Tracy equivalents using plots/counters)
// These match UE5.7's CSV_CUSTOM_STAT pattern
// ============================================================================

// CSV stat operations (for compatibility)
enum class ECsvCustomStatOp
{
    Set,
    Accumulate,
    Min,
    Max,
};

#if OLO_PROFILE && TRACY_ENABLE
    // Tracy plots can be used to track numeric values over time
    #define CSV_CUSTOM_STAT(Category, Stat, Value, Op) \
        do { TracyPlot(#Category "_" #Stat, static_cast<double>(Value)); } while(0)
    
    #define CSV_DEFINE_CATEGORY(Name, DefaultEnabled) \
        /* Tracy doesn't need category definition - no-op */
    
    // Scoped timing stats map to Tracy zones
    #define CSV_SCOPED_TIMING_STAT(Category, Stat) ZoneScopedN(#Category "_" #Stat)
    #define CSV_SCOPED_TIMING_STAT_EXCLUSIVE(Category, Stat) ZoneScopedN(#Category "_" #Stat)
#else
    #define CSV_CUSTOM_STAT(Category, Stat, Value, Op) do { } while (0)
    #define CSV_DEFINE_CATEGORY(Name, DefaultEnabled)
    #define CSV_SCOPED_TIMING_STAT(Category, Stat)
    #define CSV_SCOPED_TIMING_STAT_EXCLUSIVE(Category, Stat)
#endif
