// @file QueuedThreadPool.h
// @brief Thread pool interfaces and implementations for queued work
// 
// Ported from Unreal Engine 5.7 - Epic Games, Inc.
// 
// This file provides several thread pool implementations:
// - FQueuedThreadPool: Abstract interface
// - FQueuedThreadPoolScheduler: Uses the low-level scheduler (recommended)

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Async/QueuedWork.h"
#include "OloEngine/Async/ManualResetEvent.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/Scheduler.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Templates/Function.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <atomic>
#include <algorithm>
#include <array>

namespace OLO
{
	// Higher priority work is picked up first by the thread pool.
	enum class EQueuedWorkPriority : u8
	{
		Blocking = 0,  // Highest - for blocking operations
		Highest = 1,
		High = 2,
		Normal = 3,
		Low = 4,
		Lowest = 5,
		Count
	};

	inline const char* ToString(EQueuedWorkPriority priority)
	{
		switch (priority)
		{
		case EQueuedWorkPriority::Blocking: return "Blocking";
		case EQueuedWorkPriority::Highest:  return "Highest";
		case EQueuedWorkPriority::High:     return "High";
		case EQueuedWorkPriority::Normal:   return "Normal";
		case EQueuedWorkPriority::Low:      return "Low";
		case EQueuedWorkPriority::Lowest:   return "Lowest";
		default:                            return "Unknown";
		}
	}

	// @brief Priority queue for thread pool work items.
	// This class is NOT thread-safe and must be properly protected.
	class FThreadPoolPriorityQueue
	{
	public:
		FThreadPoolPriorityQueue() = default;

		/**
		 * Enqueue a work item at specified priority
		 */
		void Enqueue(IQueuedWork* work, EQueuedWorkPriority priority = EQueuedWorkPriority::Normal)
		{
			m_PriorityQueues[static_cast<size_t>(priority)].Add(work);
			m_NumQueuedWork.fetch_add(1, std::memory_order_relaxed);
			
			// Update first non-empty queue index
			size_t priorityIndex = static_cast<size_t>(priority);
			if (priorityIndex < m_FirstNonEmptyQueueIndex)
			{
				m_FirstNonEmptyQueueIndex = priorityIndex;
			}
		}

		/**
		 * Search and remove a queued work item from the list
		 * @return true if the work was found and removed
		 */
		bool Retract(IQueuedWork* work)
		{
			for (size_t i = 0; i < static_cast<size_t>(EQueuedWorkPriority::Count); ++i)
			{
				auto& queue = m_PriorityQueues[i];
				auto it = std::find(queue.begin(), queue.end(), work);
				if (it != queue.end())
				{
					queue.RemoveAt(static_cast<i32>(it - queue.begin()));
					m_NumQueuedWork.fetch_sub(1, std::memory_order_relaxed);
					UpdateFirstNonEmptyIndex();
					return true;
				}
			}
			return false;
		}

		/**
		 * Get the next work item in priority order.
		 * @param outPriority Optional output for the priority of the dequeued work
		 * @return Work item or nullptr if queue is empty
		 */
		IQueuedWork* Dequeue(EQueuedWorkPriority* outPriority = nullptr)
		{
			for (size_t i = m_FirstNonEmptyQueueIndex; i < static_cast<size_t>(EQueuedWorkPriority::Count); ++i)
			{
				auto& queue = m_PriorityQueues[i];
				if (!queue.IsEmpty())
				{
					IQueuedWork* work = queue[0];
					queue.RemoveAt(0);
					m_NumQueuedWork.fetch_sub(1, std::memory_order_relaxed);
					
					if (outPriority)
					{
						*outPriority = static_cast<EQueuedWorkPriority>(i);
					}
					
					UpdateFirstNonEmptyIndex();
					return work;
				}
			}
			return nullptr;
		}

		/**
		 * Get the next work item in priority order without dequeuing.
		 */
		IQueuedWork* Peek(EQueuedWorkPriority* outPriority = nullptr) const
		{
			for (size_t i = m_FirstNonEmptyQueueIndex; i < static_cast<size_t>(EQueuedWorkPriority::Count); ++i)
			{
				const auto& queue = m_PriorityQueues[i];
				if (!queue.IsEmpty())
				{
					if (outPriority)
					{
						*outPriority = static_cast<EQueuedWorkPriority>(i);
					}
					return queue[0];
				}
			}
			return nullptr;
		}

		/**
		 * Empty the queue.
		 */
		void Reset()
		{
			for (auto& queue : m_PriorityQueues)
			{
				queue.Empty();
			}
			m_NumQueuedWork.store(0, std::memory_order_relaxed);
			m_FirstNonEmptyQueueIndex = 0;
		}

		/**
		 * Get the total number of queued items.
		 */
		i32 Num() const { return m_NumQueuedWork.load(std::memory_order_relaxed); }

		/**
		 * Sort a specific priority bucket
		 */
		template<typename Predicate>
		void Sort(EQueuedWorkPriority bucket, Predicate&& predicate)
		{
			auto& queue = m_PriorityQueues[static_cast<size_t>(bucket)];
			std::sort(queue.begin(), queue.end(), std::forward<Predicate>(predicate));
		}

	private:
		void UpdateFirstNonEmptyIndex()
		{
			for (size_t i = 0; i < static_cast<size_t>(EQueuedWorkPriority::Count); ++i)
			{
				if (!m_PriorityQueues[i].IsEmpty())
				{
					m_FirstNonEmptyQueueIndex = i;
					return;
				}
			}
			m_FirstNonEmptyQueueIndex = static_cast<size_t>(EQueuedWorkPriority::Count);
		}

		size_t m_FirstNonEmptyQueueIndex = 0;
		std::array<TArray<IQueuedWork*>, static_cast<size_t>(EQueuedWorkPriority::Count)> m_PriorityQueues;
		std::atomic<i32> m_NumQueuedWork{ 0 };
	};

	/**
	 * Abstract interface for queued thread pools.
	 *
	 * This interface is used by all queued thread pools. It is used as a callback by
	 * worker threads and is used to queue asynchronous work for callers.
	 */
	class FQueuedThreadPool
	{
	public:
		virtual ~FQueuedThreadPool() = default;

		/**
		 * Creates the thread pool with the specified number of threads
		 *
		 * @param numThreads    Number of threads to use in the pool
		 * @param stackSize     Stack size for each thread (32K default)
		 * @param threadPriority Priority of pool threads
		 * @param name          Optional name for the pool (for instrumentation)
		 * @return true if the pool was created successfully
		 */
		virtual bool Create(u32 numThreads, u32 stackSize = 32 * 1024, 
			EThreadPriority threadPriority = EThreadPriority::TPri_Normal,
			const char* name = "UnknownThreadPool") = 0;

		/** Tells the pool to clean up all background threads */
		virtual void Destroy() = 0;

		/**
		 * Checks to see if there is a thread available to perform the task. If not,
		 * it queues the work for later. Otherwise it is immediately dispatched.
		 *
		 * @param work     The work that needs to be done asynchronously
		 * @param priority The priority at which to process this task
		 */
		virtual void AddQueuedWork(IQueuedWork* work, 
			EQueuedWorkPriority priority = EQueuedWorkPriority::Normal) = 0;

		/**
		 * Attempts to retract a previously queued task.
		 *
		 * @param work The work to try to retract
		 * @return true if the work was retracted before execution started
		 */
		virtual bool RetractQueuedWork(IQueuedWork* work) = 0;

		/**
		 * Get the number of threads in the pool
		 */
		virtual i32 GetNumThreads() const = 0;

		/**
		 * Allocates a thread pool (factory method)
		 * Returns the scheduler-based implementation by default.
		 */
		static FQueuedThreadPool* Allocate();

		/**
		 * Stack size override for threads created for the thread pool.
		 * Can be overridden by projects. If 0, uses the value passed to Create().
		 */
		static u32 OverrideStackSize;
	};

	/**
	 * Thread pool implementation that uses the low-level task scheduler.
	 * 
	 * This is the recommended implementation as it shares workers with the
	 * task graph and provides efficient work stealing.
	 */
	class FQueuedThreadPoolScheduler final : public FQueuedThreadPool
	{
	public:
		using PriorityMapper = TFunction<EQueuedWorkPriority(EQueuedWorkPriority)>;

		/**
		 * Constructor
		 * @param priorityMapper Optional function to remap priorities
		 * @param scheduler      The scheduler to use (defaults to global scheduler)
		 */
		explicit FQueuedThreadPoolScheduler(
			PriorityMapper priorityMapper = nullptr,
			LowLevelTasks::FScheduler* scheduler = nullptr);

		~FQueuedThreadPoolScheduler() override;

		// Disable copy/move
		FQueuedThreadPoolScheduler(const FQueuedThreadPoolScheduler&) = delete;
		FQueuedThreadPoolScheduler& operator=(const FQueuedThreadPoolScheduler&) = delete;
		FQueuedThreadPoolScheduler(FQueuedThreadPoolScheduler&&) = delete;
		FQueuedThreadPoolScheduler& operator=(FQueuedThreadPoolScheduler&&) = delete;

		/**
		 * Pause scheduling - queued tasks are held until resumed
		 */
		void Pause();

		/**
		 * Resume scheduling
		 * @param numWork Number of queued work items to release, or -1 to unpause fully
		 */
		void Resume(i32 numWork = -1);

		// FQueuedThreadPool interface
		bool Create(u32 numThreads, u32 stackSize = 32 * 1024,
			EThreadPriority threadPriority = EThreadPriority::TPri_Normal,
			const char* name = "UnknownThreadPool") override;

		void Destroy() override;

		void AddQueuedWork(IQueuedWork* work, 
			EQueuedWorkPriority priority = EQueuedWorkPriority::Normal) override;

		bool RetractQueuedWork(IQueuedWork* work) override;

		i32 GetNumThreads() const override;

	private:
		/** Internal data stored with each work item for cancellation support */
		struct FWorkInternalData : IQueuedWorkInternalData
		{
			LowLevelTasks::FTask Task;

			bool Retract() override
			{
				return Task.TryCancel();
			}
		};

		void ScheduleTasks(bool wakeUpWorker);
		void FinalizeExecution();
		FWorkInternalData* Dequeue();
		void Enqueue(EQueuedWorkPriority priority, FWorkInternalData* item);

		static LowLevelTasks::ETaskPriority MapPriority(EQueuedWorkPriority priority);

		LowLevelTasks::FScheduler* m_Scheduler = nullptr;
		PriorityMapper m_PriorityMapper;

		std::array<TArray<FWorkInternalData*>, static_cast<size_t>(EQueuedWorkPriority::Count)> m_PendingWork;
		FMutex m_PendingWorkMutex;

		std::atomic<u32> m_TaskCount{ 0 };
		std::atomic<bool> m_bIsExiting{ false };
		std::atomic<bool> m_bIsPaused{ false };
		FManualResetEvent m_Finished;
	};

	// Global thread pools (initialized by engine startup)
	extern FQueuedThreadPool* GThreadPool;
	extern FQueuedThreadPool* GIOThreadPool;
	extern FQueuedThreadPool* GBackgroundPriorityThreadPool;

} // namespace OLO
