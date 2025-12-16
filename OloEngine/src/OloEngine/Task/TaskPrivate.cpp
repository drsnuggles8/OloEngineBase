// TaskPrivate.cpp - High-level task system implementation
// Ported from UE5.7 Tasks/TaskPrivate.cpp

#include "OloEngine/Task/TaskPrivate.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Core/Log.h"

#include <limits>

namespace OloEngine::Tasks::Private
{
	// ============================================================================
	// Task Retraction Tracking (TLS)
	// ============================================================================

	/**
	 * @brief Thread-local counter for nested retraction calls
	 * 
	 * This tracks how deep we are in TryRetractAndExecute calls on the current
	 * thread. Used to detect and prevent re-entrant retraction scenarios.
	 */
	static thread_local u32 TaskRetractionRecursion = 0;

	bool IsThreadRetractingTask()
	{
		return TaskRetractionRecursion != 0;
	}

	FThreadLocalRetractionScope::FThreadLocalRetractionScope()
	{
		OLO_CORE_ASSERT(TaskRetractionRecursion != std::numeric_limits<decltype(TaskRetractionRecursion)>::max() - 1,
			"TaskRetractionRecursion overflow");
		++TaskRetractionRecursion;
	}

	FThreadLocalRetractionScope::~FThreadLocalRetractionScope()
	{
		OLO_CORE_ASSERT(TaskRetractionRecursion != 0, "TaskRetractionRecursion underflow");
		--TaskRetractionRecursion;
	}
	/**
	 * @brief Translate task priorities to named thread dispatch parameters
	 * 
	 * This function matches UE5.7's TranslatePriority behavior - it determines
	 * if a task should be routed to a named thread queue based on its
	 * EExtendedTaskPriority.
	 * 
	 * @param Priority The base task priority (currently unused but kept for API compatibility)
	 * @param ExtendedPriority The extended priority which may specify a named thread
	 * @param OutNamedThread [out] The named thread this task should execute on
	 * @param OutIsHighPriority [out] Whether this is a high-priority task
	 * @param OutIsLocalQueue [out] Whether this uses the thread-local queue
	 * @return true if this task should be routed to a named thread
	 */
	bool TranslatePriority(
		ETaskPriority Priority,
		EExtendedTaskPriority ExtendedPriority,
		ENamedThread& OutNamedThread,
		bool& OutIsHighPriority,
		bool& OutIsLocalQueue)
	{
		// Check if this is a named thread priority
		if (!IsNamedThreadPriority(ExtendedPriority))
		{
			return false;
		}

		// Determine target thread
		OutNamedThread = GetNamedThread(ExtendedPriority);
		if (OutNamedThread == ENamedThread::Invalid)
		{
			return false;
		}

		// Determine priority level (high vs normal)
		OutIsHighPriority = IsHighPriority(ExtendedPriority);

		// Determine queue type (local vs main)
		OutIsLocalQueue = IsLocalQueue(ExtendedPriority);

		return true;
	}

	/**
	 * @brief Check if we're currently on the rendering thread
	 * 
	 * This matches UE5.7's IsInRenderingThread() behavior, used to avoid
	 * incorrect CPU profiler event nesting. In UE, the RenderThread emits
	 * BeginFrameRenderThread/EndFrameRenderThread events, and task execution
	 * events would incorrectly close these frame events.
	 * 
	 * @return true if executing on the render thread
	 */
	bool IsInRenderingThread()
	{
		// Check if current thread is the render thread
		ENamedThread CurrentThread = FNamedThreadManager::Get().GetCurrentThreadIfKnown();
		return CurrentThread == ENamedThread::RenderThread;
	}

} // namespace OloEngine::Tasks::Private