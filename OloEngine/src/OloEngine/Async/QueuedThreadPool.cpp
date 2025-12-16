/**
 * @file QueuedThreadPool.cpp
 * @brief Thread pool implementation using the low-level scheduler
 * 
 * Ported from Unreal Engine 5.7 - Epic Games, Inc.
 */

#include "OloEngine/Async/QueuedThreadPool.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OLO
{
	// Global thread pools - initialized by engine startup
	FQueuedThreadPool* GThreadPool = nullptr;
	FQueuedThreadPool* GIOThreadPool = nullptr;
	FQueuedThreadPool* GBackgroundPriorityThreadPool = nullptr;

	// Static member definitions
	u32 FQueuedThreadPool::OverrideStackSize = 0;

	FQueuedThreadPool* FQueuedThreadPool::Allocate()
	{
		return new FQueuedThreadPoolScheduler();
	}

	// ============================================================================
	// FQueuedThreadPoolScheduler Implementation
	// ============================================================================

	FQueuedThreadPoolScheduler::FQueuedThreadPoolScheduler(
		PriorityMapper priorityMapper,
		LowLevelTasks::FScheduler* scheduler)
		: m_Scheduler(scheduler)
		, m_PriorityMapper(std::move(priorityMapper))
	{
		// Default priority mapper just passes through
		if (!m_PriorityMapper)
		{
			m_PriorityMapper = [](EQueuedWorkPriority p) { return p; };
		}
	}

	FQueuedThreadPoolScheduler::~FQueuedThreadPoolScheduler()
	{
		Destroy();
	}

	bool FQueuedThreadPoolScheduler::Create(u32 /*numThreads*/, u32 /*stackSize*/,
		EThreadPriority /*threadPriority*/, const char* /*name*/)
	{
		// Get the global scheduler if none was provided
		if (!m_Scheduler)
		{
			m_Scheduler = &LowLevelTasks::FScheduler::Get();
		}
		return true;
	}

	void FQueuedThreadPoolScheduler::Destroy()
	{
		m_bIsExiting.store(true, std::memory_order_release);

		// Launch all pending work so it can complete
		while (true)
		{
			FWorkInternalData* work = Dequeue();
			if (!work)
			{
				break;
			}

			// Try to cancel it first
			if (!work->Retract())
			{
				// Couldn't cancel, must let it run
				m_TaskCount.fetch_add(1, std::memory_order_acquire);
				m_Scheduler->TryLaunch(work->Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference);
			}
			else
			{
				// Successfully retracted - still need to count it for completion
				m_TaskCount.fetch_add(1, std::memory_order_acquire);
				m_Scheduler->TryLaunch(work->Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference);
			}
		}

		// Wait for all tasks to complete
		if (m_TaskCount.load(std::memory_order_acquire) != 0)
		{
			m_Finished.Wait();
		}
	}

	void FQueuedThreadPoolScheduler::Pause()
	{
		m_bIsPaused.store(true, std::memory_order_release);
	}

	void FQueuedThreadPoolScheduler::Resume(i32 numWork)
	{
		// Release specific number of work items
		for (i32 i = 0; i < numWork; ++i)
		{
			FWorkInternalData* work = Dequeue();
			if (!work)
			{
				break;
			}
			m_TaskCount.fetch_add(1, std::memory_order_acquire);
			m_Scheduler->TryLaunch(work->Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference);
		}

		// Full unpause
		if (numWork == -1)
		{
			m_bIsPaused.store(false, std::memory_order_release);
		}

		bool wakeUpWorker = true;
		ScheduleTasks(wakeUpWorker);
	}

	void FQueuedThreadPoolScheduler::AddQueuedWork(IQueuedWork* inWork, EQueuedWorkPriority inPriority)
	{
		OLO_CORE_ASSERT(!m_bIsExiting.load(std::memory_order_acquire), "Cannot add work while pool is exiting");

		// Create internal data for tracking/cancellation
		FWorkInternalData* internalData = new FWorkInternalData();
		inWork->InternalData = FQueuedWorkInternalDataRef(internalData);

		EQueuedWorkPriority priority = m_PriorityMapper(inPriority);
		LowLevelTasks::ETaskPriority taskPriority = MapPriority(priority);

		// Determine task flags
		LowLevelTasks::ETaskFlags flags = LowLevelTasks::ETaskFlags::DefaultFlags;
		if ((inWork->GetQueuedWorkFlags() & EQueuedWorkFlags::DoNotRunInsideBusyWait) != EQueuedWorkFlags::None)
		{
			// Remove busy waiting flag if work doesn't want to run inside busy waits
			flags = static_cast<LowLevelTasks::ETaskFlags>(
				static_cast<u32>(flags) & ~static_cast<u32>(LowLevelTasks::ETaskFlags::AllowBusyWaiting));
		}

		// Capture what we need for the lambda
		FQueuedThreadPoolScheduler* pool = this;
		FQueuedWorkInternalDataRef capturedInternalData = inWork->InternalData;

		// Initialize the task
		internalData->Task.Init(
			inWork->GetDebugName() ? inWork->GetDebugName() : "QueuedPoolTask",
			taskPriority,
			[inWork, capturedInternalData, pool]()
			{
				inWork->DoThreadedWork();
				pool->FinalizeExecution();
			},
			flags
		);

		if (!m_bIsPaused.load(std::memory_order_acquire))
		{
			m_TaskCount.fetch_add(1, std::memory_order_acquire);
			m_Scheduler->TryLaunch(internalData->Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference);
		}
		else
		{
			Enqueue(priority, internalData);
		}
	}

	bool FQueuedThreadPoolScheduler::RetractQueuedWork(IQueuedWork* inWork)
	{
		bool cancelled = false;
		if (inWork->InternalData.IsValid())
		{
			cancelled = inWork->InternalData->Retract();
			inWork->InternalData = nullptr;
		}

		bool wakeUpWorker = true;
		ScheduleTasks(wakeUpWorker);
		return cancelled;
	}

	i32 FQueuedThreadPoolScheduler::GetNumThreads() const
	{
		return m_Scheduler ? m_Scheduler->GetNumWorkers() : 0;
	}

	void FQueuedThreadPoolScheduler::ScheduleTasks(bool wakeUpWorker)
	{
		while (!m_bIsPaused.load(std::memory_order_acquire))
		{
			FWorkInternalData* work = Dequeue();
			if (work)
			{
				m_TaskCount.fetch_add(1, std::memory_order_acquire);
				m_Scheduler->TryLaunch(
					work->Task, 
					wakeUpWorker ? LowLevelTasks::EQueuePreference::GlobalQueuePreference 
					             : LowLevelTasks::EQueuePreference::LocalQueuePreference,
					wakeUpWorker
				);
				wakeUpWorker = true;
			}
			else
			{
				break;
			}
		}
	}

	void FQueuedThreadPoolScheduler::FinalizeExecution()
	{
		if (m_TaskCount.fetch_sub(1, std::memory_order_release) == 1 && m_bIsExiting.load(std::memory_order_acquire))
		{
			m_Finished.Notify();
		}
		else
		{
			bool wakeUpWorker = false;
			ScheduleTasks(wakeUpWorker);
		}
	}

	FQueuedThreadPoolScheduler::FWorkInternalData* FQueuedThreadPoolScheduler::Dequeue()
	{
		FUniqueLock lock(m_PendingWorkMutex);
		
		for (size_t i = 0; i < static_cast<size_t>(EQueuedWorkPriority::Count); ++i)
		{
			auto& queue = m_PendingWork[i];
			if (!queue.IsEmpty())
			{
				FWorkInternalData* work = queue[0];
				queue.RemoveAt(0);
				return work;
			}
		}
		return nullptr;
	}

	void FQueuedThreadPoolScheduler::Enqueue(EQueuedWorkPriority priority, FWorkInternalData* item)
	{
		FUniqueLock lock(m_PendingWorkMutex);
		m_PendingWork[static_cast<size_t>(priority)].Add(item);
	}

	LowLevelTasks::ETaskPriority FQueuedThreadPoolScheduler::MapPriority(EQueuedWorkPriority priority)
	{
		// Map EQueuedWorkPriority to LowLevelTasks::ETaskPriority
		switch (priority)
		{
		case EQueuedWorkPriority::Blocking:
		case EQueuedWorkPriority::Highest:
			return LowLevelTasks::ETaskPriority::High;
		case EQueuedWorkPriority::High:
			return LowLevelTasks::ETaskPriority::BackgroundHigh;
		case EQueuedWorkPriority::Normal:
			return LowLevelTasks::ETaskPriority::BackgroundNormal;
		case EQueuedWorkPriority::Low:
		case EQueuedWorkPriority::Lowest:
			return LowLevelTasks::ETaskPriority::BackgroundLow;
		default:
			return LowLevelTasks::ETaskPriority::BackgroundNormal;
		}
	}

} // namespace OLO
