// ParallelFor.h - Parallel iteration utilities
// Ported from UE5.7 Async/ParallelFor.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TaskTag.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Task/InheritedContext.h"
#include "OloEngine/Memory/MemStack.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ArrayView.h"
#include "OloEngine/Experimental/ConcurrentLinearAllocator.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/Core/PlatformTime.h"
#include "OloEngine/Misc/EnumClassFlags.h"
#include "OloEngine/Misc/Fork.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Templates/RefCounting.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <atomic>
#include <cmath>

// UE macro compatibility - TYPE_OF_NULLPTR is decltype(nullptr) aka std::nullptr_t
#ifndef TYPE_OF_NULLPTR
#define TYPE_OF_NULLPTR std::nullptr_t
#endif

namespace OloEngine
{
	// Global configuration variables (defined in ParallelFor.cpp)
	extern i32 GParallelForBackgroundYieldingTimeoutMs;
	extern bool GParallelForDisableOversubscription;

	/**
	 * @brief Returns true if the application should use threading for performance.
	 * 
	 * This is analogous to UE5.7's FApp::ShouldUseThreadingForPerformance().
	 * Returns false if threading is disabled (e.g., single-core systems, debugging).
	 */
	bool ShouldUseThreadingForPerformance();

	/**
	 * @enum EParallelForFlags
	 * @brief Flags controlling ParallelFor behavior
	 */
	enum class EParallelForFlags
	{
		// Default behavior
		None = 0,
		
		// Force single-threaded execution (mostly for testing)
		ForceSingleThread = 1 << 0,
		
		// Use unbalanced work distribution for tasks with highly variable computational time
		// Offers better work distribution among threads at the cost of a little bit more synchronization.
		Unbalanced = 1 << 1,
		
		// If running on the rendering thread, make sure the ProcessThread is called when idle
		// This prevents deadlocks when ParallelFor is called from render thread
		PumpRenderingThread = 1 << 2,
		
		// Use background priority threads
		BackgroundPriority = 1 << 3,
	};
	ENUM_CLASS_FLAGS(EParallelForFlags)

	namespace ParallelForImpl
	{
		// Helper to call body with context reference
		template <typename FunctionType, typename ContextType>
		inline void CallBody(const FunctionType& Body, const TArrayView<ContextType>& Contexts, i32 TaskIndex, i32 Index)
		{
			Body(Contexts[TaskIndex], Index);
		}

		// Helper specialization for "no context", which changes the assumed body call signature
		template <typename FunctionType>
		inline void CallBody(const FunctionType& Body, const TArrayView<TYPE_OF_NULLPTR>&, i32, i32 Index)
		{
			Body(Index);
		}

		inline i32 GetNumberOfThreadTasks(i32 Num, i32 MinBatchSize, EParallelForFlags Flags)
		{
			i32 NumThreadTasks = 0;
			
			// Check if multithreading should be used for performance (matches UE5.7 FApp::ShouldUseThreadingForPerformance())
			const bool bIsMultithread = ShouldUseThreadingForPerformance() || FForkProcessHelper::IsForkedMultithreadInstance();
			
			if (Num > 1 && (Flags & EParallelForFlags::ForceSingleThread) == EParallelForFlags::None && bIsMultithread)
			{
				NumThreadTasks = FMath::Min(
					static_cast<i32>(LowLevelTasks::FScheduler::Get().GetNumWorkers()),
					(Num + (MinBatchSize / 2)) / MinBatchSize
				);
			}

			if (!LowLevelTasks::FScheduler::Get().IsWorkerThread())
			{
				NumThreadTasks++; // Named threads help with the work
			}

			// Don't go wider than number of cores
			NumThreadTasks = FMath::Min(NumThreadTasks, static_cast<i32>(FPlatformMisc::NumberOfCoresIncludingHyperthreads()));

			return FMath::Max(NumThreadTasks, 1);
		}

		/**
		 * @brief Internal parallel for implementation with all UE5.7 optimizations
		 * 
		 * Features:
		 * - Dynamic worker launch (workers launched as needed)
		 * - Save last batch for master (avoids event wait in many cases)
		 * - Background priority yielding (reschedules after timeout)
		 * - Per-worker context support
		 * - Prework support
		 * - Priority inheritance from task tags (latency-sensitive detection)
		 */
		template<typename BodyType, typename PreWorkType, typename ContextType>
		void ParallelForInternal(
			const char* DebugName,
			i32 Num,
			i32 MinBatchSize,
			BodyType Body,
			PreWorkType CurrentThreadWorkToDoBeforeHelping,
			EParallelForFlags Flags,
			const TArrayView<ContextType>& Contexts)
		{
			if (Num == 0)
			{
				// Contract is that prework should always be called even when number of tasks is 0
				CurrentThreadWorkToDoBeforeHelping();
				return;
			}

			OLO_PROFILE_SCOPE("ParallelFor");
			OLO_CORE_ASSERT(Num >= 0, "ParallelFor: Num must be non-negative");

			i32 NumWorkers = GetNumberOfThreadTasks(Num, MinBatchSize, Flags);

			if (!Contexts.IsEmpty())
			{
				// Use at most as many workers as there are contexts when task contexts are used
				NumWorkers = FMath::Min(NumWorkers, Contexts.Num());
			}

			// Single-threaded mode
			if (NumWorkers <= 1)
			{
				// Do the prework
				CurrentThreadWorkToDoBeforeHelping();
				// No threads, just do it and return
				for (i32 Index = 0; Index < Num; Index++)
				{
					CallBody(Body, Contexts, 0, Index);
				}
				return;
			}

			// Calculate batch sizes
			i32 BatchSize = 1;
			i32 NumBatches = Num;
			const bool bIsUnbalanced = (Flags & EParallelForFlags::Unbalanced) != EParallelForFlags::None;
			
			if (!bIsUnbalanced)
			{
				for (i32 Div = 6; Div; Div--)
				{
					if (Num >= (NumWorkers * Div))
					{
						BatchSize = FMath::DivideAndRoundUp(Num, NumWorkers * Div);
						NumBatches = FMath::DivideAndRoundUp(Num, BatchSize);

						if (NumBatches >= NumWorkers)
						{
							break;
						}
					}
				}
			}
			NumWorkers--; // Decrement one because this function will work on it locally

			OLO_CORE_ASSERT(BatchSize * NumBatches >= Num, "Batch calculation error");

			// Determine priority with latency-sensitive task inheritance (matches UE5.7)
			// Anything scheduled by latency-sensitive threads (game thread, render thread, etc.)
			// should use high priority unless explicitly set to background.
			const OLO::ETaskTag LatencySensitiveTasks = 
				OLO::ETaskTag::EStaticInit |
				OLO::ETaskTag::EGameThread |
				OLO::ETaskTag::ESlateThread |
				OLO::ETaskTag::ERenderingThread |
				OLO::ETaskTag::ERhiThread;

			const bool bBackgroundPriority = (Flags & EParallelForFlags::BackgroundPriority) != EParallelForFlags::None;
			const bool bIsLatencySensitive = (OLO::FTaskTagScope::GetCurrentTag() & LatencySensitiveTasks) != OLO::ETaskTag::ENone;

			LowLevelTasks::ETaskPriority Priority = LowLevelTasks::ETaskPriority::Inherit;
			if (bIsLatencySensitive && !bBackgroundPriority)
			{
				Priority = LowLevelTasks::ETaskPriority::High;
			}
			else if (bBackgroundPriority)
			{
				Priority = LowLevelTasks::ETaskPriority::BackgroundNormal;
			}

			// Shared data between tasks (ref-counted for safe lifetime)
			// Using TConcurrentLinearObject for efficient allocation (matches UE5.7)
			// Inherits from FInheritedContextBase to propagate TLS context to worker tasks
			struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) FParallelForData 
				: public FThreadSafeRefCountedObject
				, private FInheritedContextBase
			{
				using FInheritedContextBase::RestoreInheritedContext;

				FParallelForData(
					const char* InDebugName,
					i32 InNum,
					i32 InBatchSize,
					i32 InNumBatches,
					i32 InNumWorkers,
					const TArrayView<ContextType>& InContexts,
					const BodyType& InBody,
					FEventRef& InFinishedSignal,
					LowLevelTasks::ETaskPriority InPriority)
					: DebugName(InDebugName)
					, Num(InNum)
					, BatchSize(InBatchSize)
					, NumBatches(InNumBatches)
					, Contexts(InContexts)
					, Body(InBody)
					, FinishedSignal(InFinishedSignal)
					, Priority(InPriority)
				{
					// Capture the inherited context from the launching thread
					// This allows worker tasks to inherit LLM tags, memory trace, etc.
					CaptureInheritedContext();
					
					IncompleteBatches.store(NumBatches, std::memory_order_relaxed);
					Tasks.SetNum(InNumWorkers);
				}

				i32 GetNextWorkerIndexToLaunch()
				{
					const i32 WorkerIndex = LaunchedWorkers.fetch_add(1, std::memory_order_relaxed);
					return WorkerIndex >= Tasks.Num() ? -1 : WorkerIndex;
				}

				const char* DebugName;
				std::atomic<i32> BatchItem{ 0 };
				std::atomic<i32> IncompleteBatches{ 0 };
				std::atomic<i32> LaunchedWorkers{ 0 };
				i32 Num;
				i32 BatchSize;
				i32 NumBatches;
				const TArrayView<ContextType>& Contexts;
				const BodyType& Body;
				FEventRef& FinishedSignal;
				LowLevelTasks::ETaskPriority Priority;
				TArray<LowLevelTasks::FTask> Tasks;
			};
			using FDataHandle = TRefCountPtr<FParallelForData>;

			// Each task has an executor - matches UE5.7's FParallelExecutor pattern
			// The executor is a callable that performs the actual parallel work
			class FParallelExecutor
			{
				mutable FDataHandle Data;
				i32 WorkerIndex;
				mutable bool bReschedule = false;

			public:
				inline FParallelExecutor(FDataHandle&& InData, i32 InWorkerIndex)
					: Data(MoveTemp(InData))
					, WorkerIndex(InWorkerIndex)
				{
				}

				FParallelExecutor(const FParallelExecutor&) = delete;
				FParallelExecutor(FParallelExecutor&& Other) = default;

				~FParallelExecutor()
				{
					// If we need to reschedule (background priority yielding), do it in destructor
					if (Data.IsValid() && bReschedule)
					{
						FParallelExecutor::LaunchTask(MoveTemp(Data), WorkerIndex);
					}
				}

				inline const FDataHandle& GetData() const
				{
					return Data;
				}

				// The main work function - called as task body
				inline bool operator()(const bool bIsMaster = false) const noexcept
				{
					// Restore inherited context from the launching thread
					FInheritedContextScope InheritedContextScope = Data->RestoreInheritedContext();
					FMemMark Mark(FMemStack::Get());

					OLO_PROFILE_SCOPE("ParallelFor.Worker");

					const i32 NumBatches = Data->NumBatches;

					// We're going to consume one ourself, so we need at least 2 left to consider launching a new worker
					// We also do not launch a worker from the master as we already launched one before doing prework.
					if (bIsMaster == false && Data->BatchItem.load(std::memory_order_relaxed) + 2 <= NumBatches)
					{
						LaunchAnotherWorkerIfNeeded(Data);
					}

					// Background priority yielding setup
					f64 StartTime = 0.0;
					f64 YieldingThresholdSec = 0.0;
					const bool bIsBackgroundPriority = !bIsMaster && (Data->Priority >= LowLevelTasks::ETaskPriority::BackgroundNormal);

					if (bIsBackgroundPriority)
					{
						StartTime = FPlatformTime::Seconds();
						YieldingThresholdSec = static_cast<f64>(FMath::Max(0, GParallelForBackgroundYieldingTimeoutMs)) / 1000.0;
					}

					const i32 Num = Data->Num;
					const i32 BatchSize = Data->BatchSize;
					const TArrayView<ContextType>& Contexts = Data->Contexts;
					const BodyType& Body = Data->Body;

					const bool bSaveLastBlockForMaster = (Num > NumBatches);
					for (;;)
					{
						i32 BatchIndex = Data->BatchItem.fetch_add(1, std::memory_order_relaxed);

						// Save the last block for the master to avoid an event
						if (bSaveLastBlockForMaster && BatchIndex >= NumBatches - 1)
						{
							if (!bIsMaster)
							{
								return false;
							}
							BatchIndex = (NumBatches - 1);
						}

						i32 StartIndex = BatchIndex * BatchSize;
						i32 EndIndex = FMath::Min(StartIndex + BatchSize, Num);
						for (i32 Index = StartIndex; Index < EndIndex; Index++)
						{
							CallBody(Body, Contexts, WorkerIndex, Index);
						}

						// We need to decrement IncompleteBatches when processing a Batch because we need to know if we are the last one
						// so that if the main thread is the last one we can avoid an FEvent call.

						// Memory ordering is also very important here as it is what's making sure memory manipulated
						// by the parallelfor is properly published before exiting so that it's safe to be read
						// without other synchronization mechanism.
						if (StartIndex < Num && Data->IncompleteBatches.fetch_sub(1, std::memory_order_acq_rel) == 1)
						{
							if (!bIsMaster)
							{
								Data->FinishedSignal->Trigger();
							}

							return true;
						}
						else if (EndIndex >= Num)
						{
							return false;
						}
						else if (!bIsBackgroundPriority)
						{
							continue;
						}

						// Background priority yielding check
						const f64 PassedTime = FPlatformTime::Seconds() - StartTime;
						if (PassedTime > YieldingThresholdSec)
						{
							// Abort and reschedule to give higher priority tasks a chance to run
							bReschedule = true;
							return false;
						}
					}
				}

				static void LaunchTask(FDataHandle&& InData, i32 InWorkerIndex, bool bWakeUpWorker = true)
				{
					// CRITICAL: Store reference to task BEFORE MoveTemp invalidates InData
					// This matches UE5.7's pattern to avoid undefined behavior
					LowLevelTasks::FTask& Task = InData->Tasks[InWorkerIndex];

					const char* TaskDebugName = InData->DebugName;
					LowLevelTasks::ETaskPriority TaskPriority = InData->Priority;

					// Initialize and launch using the stored reference
					Task.Init(TaskDebugName, TaskPriority, FParallelExecutor(MoveTemp(InData), InWorkerIndex));
					[[maybe_unused]] bool bLaunched = LowLevelTasks::TryLaunch(Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference, bWakeUpWorker);
					OLO_CORE_ASSERT(bLaunched, "Failed to launch ParallelFor task");
				}

				static void LaunchAnotherWorkerIfNeeded(FDataHandle& InData)
				{
					const i32 WorkerIndex = InData->GetNextWorkerIndexToLaunch();
					if (WorkerIndex != -1)
					{
						LaunchTask(FDataHandle(InData), WorkerIndex);
					}
				}
			};

			// Launch all the worker tasks
			FEventRef FinishedSignal{ EEventMode::ManualReset };
			FDataHandle Data = new FParallelForData(DebugName, Num, BatchSize, NumBatches, NumWorkers, Contexts, Body, FinishedSignal, Priority);

			// Launch the first worker before we start doing prework
			FParallelExecutor::LaunchAnotherWorkerIfNeeded(Data);

			// Do the prework
			CurrentThreadWorkToDoBeforeHelping();

			// Help with the parallel-for to prevent deadlocks
			// Master thread uses NumWorkers as its worker index (the "extra" slot)
			FParallelExecutor LocalExecutor(MoveTemp(Data), NumWorkers);
			const bool bFinishedLast = LocalExecutor(true);

			if (!bFinishedLast)
			{
				OLO_PROFILE_SCOPE("ParallelFor.Wait");

				// Check if we need to pump the rendering thread while waiting
				const bool bPumpRenderingThread = (Flags & EParallelForFlags::PumpRenderingThread) != EParallelForFlags::None;
				if (bPumpRenderingThread && OLO::IsInActualRenderingThread())
				{
					// Pump the rendering thread to prevent deadlocks
					// TODO: Once TaskGraphInterface is implemented, use:
					// while (!FinishedSignal->Wait(1))
					// {
					//     FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GetRenderThread_Local());
					// }
					// For now, just wait with periodic yields
					while (!FinishedSignal->Wait(1))
					{
						// Yield to allow other rendering work to proceed
						FPlatformProcess::Yield();
					}
				}
				else if (GParallelForDisableOversubscription)
				{
					LowLevelTasks::Private::FOversubscriptionAllowedScope Scope(false);
					FinishedSignal->Wait();
				}
				else
				{
					// This can spawn new threads to handle tasks
					FinishedSignal->Wait();
				}
			}

			OLO_CORE_ASSERT(LocalExecutor.GetData()->BatchItem.load(std::memory_order_relaxed) * LocalExecutor.GetData()->BatchSize >= LocalExecutor.GetData()->Num,
				"ParallelFor: Not all work was completed");
		}

	} // namespace ParallelForImpl

	// ========================================================================
	// Basic ParallelFor overloads
	// ========================================================================

	/**
	 * @brief Execute a function in parallel for each index in a range
	 * 
	 * @param Num Number of iterations (0 to Num-1)
	 * @param Body Function to call for each index, signature: void(i32 Index)
	 * @param Flags Optional flags to control execution
	 * 
	 * Example:
	 * @code
	 * TArray<float> Data(1000);
	 * ParallelFor(Data.Num(), [&Data](i32 Index)
	 * {
	 *     Data[Index] = FMath::Sqrt(static_cast<float>(Index));
	 * });
	 * @endcode
	 */
	template<typename BodyType>
	void ParallelFor(i32 Num, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	/**
	 * @brief Execute a function in parallel with debug name
	 */
	template<typename BodyType>
	void ParallelFor(const char* DebugName, i32 Num, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal(DebugName, Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	/**
	 * @brief Execute a function in parallel with specified minimum batch size
	 */
	template<typename BodyType>
	void ParallelFor(const char* DebugName, i32 Num, i32 MinBatchSize, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), [](){}, Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	// ========================================================================
	// ParallelForWithPreWork - execute prework on master before helping
	// ========================================================================

	/**
	 * @brief Execute a function in parallel with prework on the master thread
	 * 
	 * The prework function is executed on the calling thread before it starts
	 * helping with the parallel work. This is useful for setup operations.
	 * 
	 * @param Num Number of iterations
	 * @param Body Function to call for each index
	 * @param CurrentThreadWorkToDoBeforeHelping Prework function
	 * @param Flags Optional flags
	 */
	template<typename BodyType, typename PreWorkType>
	void ParallelForWithPreWork(i32 Num, BodyType&& Body, PreWorkType&& CurrentThreadWorkToDoBeforeHelping, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Forward<BodyType>(Body), Forward<PreWorkType>(CurrentThreadWorkToDoBeforeHelping), Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	template<typename BodyType, typename PreWorkType>
	void ParallelForWithPreWork(const char* DebugName, i32 Num, i32 MinBatchSize, BodyType&& Body, PreWorkType&& CurrentThreadWorkToDoBeforeHelping, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), Forward<PreWorkType>(CurrentThreadWorkToDoBeforeHelping), Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	// ========================================================================
	// ParallelForWithTaskContext - per-worker context accumulation
	// ========================================================================

	/**
	 * @brief Execute a function in parallel with per-worker context objects
	 * 
	 * This variant constructs a user-defined context object for each task that may
	 * get spawned. The context is passed to the loop body, giving it a task-local
	 * "workspace" that can be mutated without need for synchronization primitives.
	 * 
	 * @param OutContexts Array that will hold the task-level context objects
	 * @param Num Number of iterations
	 * @param Body Function signature: void(ContextType& Context, i32 Index)
	 * @param Flags Optional flags
	 * 
	 * Example:
	 * @code
	 * struct FAccumulator { i64 Sum = 0; };
	 * TArray<FAccumulator> Contexts;
	 * ParallelForWithTaskContext(Contexts, Data.Num(), [&Data](FAccumulator& Ctx, i32 Index)
	 * {
	 *     Ctx.Sum += Data[Index];
	 * });
	 * // Reduce the per-worker results
	 * i64 TotalSum = 0;
	 * for (const auto& Ctx : Contexts) TotalSum += Ctx.Sum;
	 * @endcode
	 */
	template<typename ContextType, typename BodyType>
	void ParallelForWithTaskContext(TArray<ContextType>& OutContexts, i32 Num, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, 1, Flags);
			OutContexts.Reset();
			OutContexts.AddDefaulted(NumContexts);
			ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with per-worker context objects and custom constructor
	 * 
	 * @param OutContexts Array that will hold the task-level context objects
	 * @param Num Number of iterations
	 * @param ContextConstructor Function to construct each context: ContextType(i32 ContextIndex, i32 NumContexts)
	 * @param Body Function signature: void(ContextType& Context, i32 Index)
	 * @param Flags Optional flags
	 */
	template<typename ContextType, typename ContextConstructorType, typename BodyType>
	void ParallelForWithTaskContext(TArray<ContextType>& OutContexts, i32 Num, ContextConstructorType&& ContextConstructor, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, 1, Flags);
			OutContexts.Reset();
			OutContexts.Reserve(NumContexts);
			for (i32 ContextIndex = 0; ContextIndex < NumContexts; ++ContextIndex)
			{
				OutContexts.Emplace(ContextConstructor(ContextIndex, NumContexts));
			}
			ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with per-worker context objects (named)
	 */
	template<typename ContextType, typename BodyType>
	void ParallelForWithTaskContext(const char* DebugName, TArray<ContextType>& OutContexts, i32 Num, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, 1, Flags);
			OutContexts.Reset();
			OutContexts.AddDefaulted(NumContexts);
			ParallelForImpl::ParallelForInternal(DebugName, Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with per-worker context objects (named, with batch size)
	 */
	template<typename ContextType, typename BodyType>
	void ParallelForWithTaskContext(const char* DebugName, TArray<ContextType>& OutContexts, i32 Num, i32 MinBatchSize, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, MinBatchSize, Flags);
			OutContexts.Reset();
			OutContexts.AddDefaulted(NumContexts);
			ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), [](){}, Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with per-worker context objects (named, with batch size and constructor)
	 */
	template<typename ContextType, typename ContextConstructorType, typename BodyType>
	void ParallelForWithTaskContext(const char* DebugName, TArray<ContextType>& OutContexts, i32 Num, i32 MinBatchSize, ContextConstructorType&& ContextConstructor, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, MinBatchSize, Flags);
			OutContexts.Reset();
			OutContexts.Reserve(NumContexts);
			for (i32 ContextIndex = 0; ContextIndex < NumContexts; ++ContextIndex)
			{
				OutContexts.Emplace(ContextConstructor(ContextIndex, NumContexts));
			}
			ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), [](){}, Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	// ========================================================================
	// ParallelForWithExistingTaskContext - use pre-allocated contexts
	// ========================================================================

	/**
	 * @brief Execute a function in parallel with user-provided context objects
	 * 
	 * This variant takes a pre-allocated array of contexts rather than creating them.
	 * 
	 * @param Contexts User-provided array of context objects (one task per context at most)
	 * @param Num Number of iterations
	 * @param MinBatchSize Minimum batch size
	 * @param Body Function signature: void(ContextType& Context, i32 Index)
	 * @param Flags Optional flags
	 */
	template<typename ContextType, typename BodyType>
	void ParallelForWithExistingTaskContext(TArrayView<ContextType> Contexts, i32 Num, i32 MinBatchSize, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal("ParallelFor", Num, MinBatchSize, Forward<BodyType>(Body), [](){}, Flags, Contexts);
	}

	/**
	 * @brief Execute a function in parallel with user-provided context objects (named)
	 */
	template<typename ContextType, typename BodyType>
	void ParallelForWithExistingTaskContext(const char* DebugName, TArrayView<ContextType> Contexts, i32 Num, i32 MinBatchSize, BodyType&& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), [](){}, Flags, Contexts);
	}

	// ========================================================================
	// ParallelForWithPreWorkWithTaskContext - combined prework + context
	// ========================================================================

	/**
	 * @brief Execute a function in parallel with prework and per-worker context objects
	 */
	template<typename ContextType, typename ContextConstructorType, typename BodyType, typename PreWorkType>
	void ParallelForWithPreWorkWithTaskContext(
		const char* DebugName,
		TArray<ContextType>& OutContexts,
		i32 Num,
		i32 MinBatchSize,
		ContextConstructorType&& ContextConstructor,
		BodyType&& Body,
		PreWorkType&& CurrentThreadWorkToDoBeforeHelping,
		EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, MinBatchSize, Flags);
			OutContexts.Reset();
			OutContexts.Reserve(NumContexts);
			for (i32 ContextIndex = 0; ContextIndex < NumContexts; ++ContextIndex)
			{
				OutContexts.Emplace(ContextConstructor(ContextIndex, NumContexts));
			}
			ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), Forward<PreWorkType>(CurrentThreadWorkToDoBeforeHelping), Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with prework and per-worker context objects (default construction)
	 */
	template<typename ContextType, typename BodyType, typename PreWorkType>
	void ParallelForWithPreWorkWithTaskContext(
		const char* DebugName,
		TArray<ContextType>& OutContexts,
		i32 Num,
		i32 MinBatchSize,
		BodyType&& Body,
		PreWorkType&& CurrentThreadWorkToDoBeforeHelping,
		EParallelForFlags Flags = EParallelForFlags::None)
	{
		if (Num > 0)
		{
			const i32 NumContexts = ParallelForImpl::GetNumberOfThreadTasks(Num, MinBatchSize, Flags);
			OutContexts.Reset();
			OutContexts.AddDefaulted(NumContexts);
			ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), Forward<PreWorkType>(CurrentThreadWorkToDoBeforeHelping), Flags, TArrayView<ContextType>(OutContexts));
		}
	}

	/**
	 * @brief Execute a function in parallel with prework and user-provided context objects
	 */
	template<typename ContextType, typename BodyType, typename PreWorkType>
	void ParallelForWithPreWorkWithExistingTaskContext(
		const char* DebugName,
		TArrayView<ContextType> Contexts,
		i32 Num,
		i32 MinBatchSize,
		BodyType&& Body,
		PreWorkType&& CurrentThreadWorkToDoBeforeHelping,
		EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal(DebugName, Num, MinBatchSize, Forward<BodyType>(Body), Forward<PreWorkType>(CurrentThreadWorkToDoBeforeHelping), Flags, Contexts);
	}

	// ========================================================================
	// Legacy ParallelFor overloads for API compatibility
	// These match UE5.7's legacy API with bool parameters
	// ========================================================================

	/**
	 * @brief Legacy ParallelFor overload with bool parameters
	 * 
	 * @param Num Number of iterations
	 * @param Body Function to call for each index
	 * @param bForceSingleThread If true, execute single-threaded
	 * @param bPumpRenderingThread If true, pump rendering thread while waiting
	 * 
	 * @note Prefer the EParallelForFlags version for new code
	 */
	template<typename BodyType>
	void ParallelFor(i32 Num, BodyType&& Body, bool bForceSingleThread, bool bPumpRenderingThread = false)
	{
		EParallelForFlags Flags = EParallelForFlags::None;
		if (bForceSingleThread)
		{
			Flags |= EParallelForFlags::ForceSingleThread;
		}
		if (bPumpRenderingThread)
		{
			Flags |= EParallelForFlags::PumpRenderingThread;
		}
		ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Forward<BodyType>(Body), [](){}, Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	/**
	 * @brief Template version of ParallelFor for explicit function type
	 * 
	 * @tparam FunctionType The callable type
	 * @param Num Number of iterations
	 * @param Body Function to call for each index
	 * @param Flags Optional flags
	 */
	template<typename FunctionType>
	void ParallelForTemplate(i32 Num, const FunctionType& Body, EParallelForFlags Flags = EParallelForFlags::None)
	{
		ParallelForImpl::ParallelForInternal("ParallelFor", Num, 1, Body, [](){}, Flags, TArrayView<TYPE_OF_NULLPTR>());
	}

	// ========================================================================
	// AutoRTFM (Real-Time Finite Memory) Transaction Check
	// ========================================================================

	/**
	 * @brief Check if we're in an AutoRTFM transaction
	 * 
	 * UE5.7 checks AutoRTFM::IsClosed() before ParallelFor and forces single-thread
	 * if inside a transaction to prevent data races. OloEngine doesn't have RTFM
	 * yet, so this is a stub that always returns false.
	 * 
	 * @return true if in an RTFM transaction (always false for now)
	 */
	inline bool IsInAutoRTFMTransaction()
	{
		// Stub - OloEngine doesn't have AutoRTFM yet
		// When implemented, this would check AutoRTFM::IsClosed()
		return false;
	}

	/**
	 * @brief Apply AutoRTFM flags to ParallelFor flags
	 * 
	 * If inside an RTFM transaction, forces single-threaded execution.
	 * 
	 * @param Flags The original flags
	 * @return Modified flags with ForceSingleThread if in transaction
	 */
	inline EParallelForFlags ApplyAutoRTFMFlags(EParallelForFlags Flags)
	{
		if (IsInAutoRTFMTransaction())
		{
			Flags |= EParallelForFlags::ForceSingleThread;
		}
		return Flags;
	}

} // namespace OloEngine
