// Task.h - High-level task API
// Ported from UE5.7 Tasks/Task.h

#pragma once

#include "OloEngine/Task/TaskPrivate.h"
#include "OloEngine/Containers/StaticArray.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/Core/MonotonicTime.h"

namespace OloEngine::Tasks
{
	// Forward declarations
	template<typename ResultType> class TTask;
	template<typename TaskCollectionType> bool Wait(const TaskCollectionType& Tasks, FMonotonicTimeSpan InTimeout);
	template<typename TaskType> void AddNested(const TaskType& Nested);

	namespace Private
	{
		// Forward declare helpers
		template<i32 Index, typename ArrayType, typename FirstTaskType, typename... OtherTasksTypes>
		void PrerequisitesUnpacker(ArrayType& Array, FirstTaskType& FirstTask, OtherTasksTypes&... OtherTasks);

		template<i32 Index, typename ArrayType, typename TaskType>
		void PrerequisitesUnpacker(ArrayType& Array, TaskType& Task);

		// ============================================================================
		// FTaskHandle - Common task handle functionality
		// ============================================================================

		class FTaskHandle
		{
			friend FTaskBase;

			template<i32 Index, typename ArrayType, typename FirstTaskType, typename... OtherTasksTypes>
			friend void PrerequisitesUnpacker(ArrayType& Array, FirstTaskType& FirstTask, OtherTasksTypes&... OtherTasks);

			template<i32 Index, typename ArrayType, typename TaskType>
			friend void PrerequisitesUnpacker(ArrayType& Array, TaskType& Task);

			template<typename TaskCollectionType>
			friend bool TryRetractAndExecute(const TaskCollectionType& Tasks, FTimeout Timeout);

			template<typename TaskCollectionType>
			friend bool OloEngine::Tasks::Wait(const TaskCollectionType& Tasks, FMonotonicTimeSpan InTimeout);

			template<typename TaskType>
			friend void OloEngine::Tasks::AddNested(const TaskType& Nested);

		protected:
			explicit FTaskHandle(FTaskBase* Other)
				: Pimpl(Other, /*bAddRef = */false)
			{}

		public:
			using FTaskHandleId = void;

			FTaskHandle() = default;

			bool IsValid() const
			{
				return Pimpl.IsValid();
			}

			bool IsCompleted() const
			{
				return !IsValid() || Pimpl->IsCompleted();
			}

			/**
			 * @brief Wait for task completion with timeout
			 * @return true if completed, false if timeout
			 */
			bool Wait(FMonotonicTimeSpan Timeout) const
			{
				return !IsValid() || Pimpl->Wait(FTimeout{ Timeout });
			}

			/**
			 * @brief Wait for task completion without timeout
			 */
			bool Wait() const
			{
				if (IsValid())
				{
					Pimpl->Wait();
				}
				return true;
			}

			/**
			 * @brief Try to retract and execute inline
			 */
			bool TryRetractAndExecute()
			{
				if (IsValid())
				{
					Pimpl->TryRetractAndExecute(FTimeout::Never());
				}
				return IsCompleted();
			}

			/**
			 * @brief Launch a task for async execution
			 */
			template<typename TaskBodyType>
			void Launch(
				const char* DebugName,
				TaskBodyType&& TaskBody,
				ETaskPriority Priority = ETaskPriority::Normal,
				EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
				ETaskFlags Flags = ETaskFlags::None
			)
			{
				OLO_CORE_ASSERT(!IsValid(), "Task already launched");

				using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;
				FExecutableTask* Task = FExecutableTask::Create(DebugName, Forward<TaskBodyType>(TaskBody), 
					Priority, ExtendedPriority, Flags);
				*Pimpl.GetInitReference() = Task;
				Task->TryLaunch(sizeof(*Task));
			}

			/**
			 * @brief Launch a task with prerequisites
			 */
			template<typename TaskBodyType, typename PrerequisitesCollectionType>
			void Launch(
				const char* DebugName,
				TaskBodyType&& TaskBody,
				PrerequisitesCollectionType&& Prerequisites,
				ETaskPriority Priority = ETaskPriority::Normal,
				EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
				ETaskFlags Flags = ETaskFlags::None
			)
			{
				OLO_CORE_ASSERT(!IsValid(), "Task already launched");

				using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;
				FExecutableTask* Task = FExecutableTask::Create(DebugName, Forward<TaskBodyType>(TaskBody),
					Priority, ExtendedPriority, Flags);
				Task->AddPrerequisites(Forward<PrerequisitesCollectionType>(Prerequisites));
				*Pimpl.GetInitReference() = Task;
				Task->TryLaunch(sizeof(*Task));
			}

			bool IsAwaitable() const
			{
				return IsValid() && Pimpl->IsAwaitable();
			}

			bool operator==(const FTaskHandle& Other) const
			{
				return Pimpl == Other.Pimpl;
			}

			bool operator!=(const FTaskHandle& Other) const
			{
				return Pimpl != Other.Pimpl;
			}

			ETaskPriority GetPriority() const
			{
				return Pimpl->GetPriority();
			}

			EExtendedTaskPriority GetExtendedPriority() const
			{
				return Pimpl->GetExtendedPriority();
			}

		//protected: // Need access for Prerequisites()
			TRefCountPtr<FTaskBase> Pimpl;
		};

	} // namespace Private

	// ============================================================================
	// TTask - Typed task handle
	// ============================================================================

	/**
	 * @class TTask
	 * @brief Movable/copyable handle to an async task with result retrieval
	 */
	template<typename ResultType>
	class TTask : public Private::FTaskHandle
	{
	public:
		TTask() = default;

		/**
		 * @brief Wait and return the task result
		 */
		ResultType& GetResult()
		{
			OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid task");
			FTaskHandle::Wait();
			return static_cast<Private::TTaskWithResult<ResultType>*>(Pimpl.GetReference())->GetResult();
		}

	private:
		friend class FPipe;

		explicit TTask(Private::FTaskBase* Other)
			: FTaskHandle(Other)
		{}
	};

	/**
	 * @brief Specialization for void tasks
	 */
	template<>
	class TTask<void> : public Private::FTaskHandle
	{
	public:
		TTask() = default;

		void GetResult()
		{
			OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid task");
			Wait();
		}

	private:
		friend class FPipe;

		explicit TTask(Private::FTaskBase* Other)
			: FTaskHandle(Other)
		{}
	};

	// ============================================================================
	// FTaskEvent - Signaling primitive
	// ============================================================================

	/**
	 * @class FTaskEvent
	 * @brief Synchronization primitive that can be used as a task prerequisite
	 */
	class FTaskEvent : public Private::FTaskHandle
	{
	public:
		explicit FTaskEvent(const char* DebugName)
			: Private::FTaskHandle(Private::FTaskEventBase::Create(DebugName))
		{
		}

		template<typename PrerequisitesType>
		void AddPrerequisites(const PrerequisitesType& Prerequisites)
		{
			Pimpl->AddPrerequisites(Prerequisites);
		}

		void Trigger()
		{
			if (!IsCompleted())
			{
				Pimpl->Trigger(sizeof(*Pimpl));
			}
		}
	};

	// Type alias for FTask
	using FTask = Private::FTaskHandle;

	// ============================================================================
	// Free function Launch
	// ============================================================================

	/**
	 * @brief Launch a task for async execution
	 */
	template<typename TaskBodyType>
	TTask<TInvokeResult_T<TaskBodyType>> Launch(
		const char* DebugName,
		TaskBodyType&& TaskBody,
		ETaskPriority Priority = ETaskPriority::Normal,
		EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
		ETaskFlags Flags = ETaskFlags::None
	)
	{
		using FResult = TInvokeResult_T<TaskBodyType>;
		TTask<FResult> Task;
		Task.Launch(DebugName, Forward<TaskBodyType>(TaskBody), Priority, ExtendedPriority, Flags);
		return Task;
	}

	/**
	 * @brief Launch a task with prerequisites
	 */
	template<typename TaskBodyType, typename PrerequisitesCollectionType>
	TTask<TInvokeResult_T<TaskBodyType>> Launch(
		const char* DebugName,
		TaskBodyType&& TaskBody,
		PrerequisitesCollectionType&& Prerequisites,
		ETaskPriority Priority = ETaskPriority::Normal,
		EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
		ETaskFlags Flags = ETaskFlags::None
	)
	{
		using FResult = TInvokeResult_T<TaskBodyType>;
		TTask<FResult> Task;
		Task.Launch(DebugName, Forward<TaskBodyType>(TaskBody), 
			Forward<PrerequisitesCollectionType>(Prerequisites),
			Priority, ExtendedPriority, Flags);
		return Task;
	}

	// ============================================================================
	// Prerequisites helper
	// ============================================================================

	namespace Private
	{
		template<i32 Index, typename ArrayType, typename FirstTaskType, typename... OtherTasksTypes>
		void PrerequisitesUnpacker(ArrayType& Array, FirstTaskType& FirstTask, OtherTasksTypes&... OtherTasks)
		{
			Array[Index] = FirstTask.Pimpl.GetReference();
			PrerequisitesUnpacker<Index + 1>(Array, OtherTasks...);
		}

		template<i32 Index, typename ArrayType, typename TaskType>
		void PrerequisitesUnpacker(ArrayType& Array, TaskType& Task)
		{
			Array[Index] = Task.Pimpl.GetReference();
		}
	}

	/**
	 * @brief Create a prerequisites array from variadic tasks
	 */
	template<typename... TaskTypes,
		typename std::decay_t<decltype(std::declval<std::tuple<TaskTypes...>>())>* = nullptr>
	TStaticArray<Private::FTaskBase*, sizeof...(TaskTypes)> Prerequisites(TaskTypes&... Tasks)
	{
		TStaticArray<Private::FTaskBase*, sizeof...(TaskTypes)> Res;
		Private::PrerequisitesUnpacker<0>(Res, Tasks...);
		return Res;
	}

	/**
	 * @brief Pass through an existing prerequisites collection
	 */
	template<typename TaskCollectionType>
	const TaskCollectionType& Prerequisites(const TaskCollectionType& Tasks)
	{
		return Tasks;
	}

	// ============================================================================
	// Wait functions
	// ============================================================================

	inline void Wait(Private::FTaskHandle& Task)
	{
		Task.Wait();
	}

	/**
	 * @brief Wait for multiple tasks with timeout
	 */
	template<typename TaskCollectionType>
	bool Wait(const TaskCollectionType& Tasks, FMonotonicTimeSpan InTimeout = FMonotonicTimeSpan::Infinity())
	{
		// Create an inline task that depends on all input tasks
		return Launch(
			"Waiting Task",
			[]() {},
			Prerequisites(Tasks),
			ETaskPriority::Default,
			EExtendedTaskPriority::Inline
		).Wait(InTimeout);
	}

	// ============================================================================
	// WaitAny / Any
	// ============================================================================

	/**
	 * @brief Wait until any task completes
	 * @return Index of first completed task, or INDEX_NONE on timeout or empty input
	 */
	template<typename TaskCollectionType>
	i32 WaitAny(const TaskCollectionType& Tasks, FMonotonicTimeSpan Timeout = FMonotonicTimeSpan::Infinity())
	{
		if (OLO_UNLIKELY(Tasks.Num() == 0))
		{
			return INDEX_NONE;
		}

		// Fast path: check if any already completed
		for (i32 Index = 0; Index < Tasks.Num(); ++Index)
		{
			if (Tasks[Index].IsCompleted())
			{
				return Index;
			}
		}

		struct FSharedData
		{
			FManualResetEvent Event;
			std::atomic<i32> CompletedTaskIndex{ 0 };
		};

		// Shared data usage is important to avoid the variable to go out of scope
		// before all the tasks have been run even if we exit after the first event
		// is triggered.
		auto SharedData = MakeShared<FSharedData>();

		for (i32 Index = 0; Index < Tasks.Num(); ++Index)
		{
			// Launch inline task that waits for input task via prerequisite
			Launch(
				"WaitAny_Helper",
				[SharedData, Index]()
				{
					SharedData->CompletedTaskIndex.store(Index, std::memory_order_relaxed);
					SharedData->Event.Notify();
				},
				Prerequisites(Tasks[Index]),
				ETaskPriority::Default,
				EExtendedTaskPriority::Inline
			);
		}

		if (SharedData->Event.WaitFor(Timeout))
		{
			return SharedData->CompletedTaskIndex.load(std::memory_order_relaxed);
		}

		return INDEX_NONE;
	}

	/**
	 * @brief Create a task that completes when any input task completes
	 */
	template<typename TaskCollectionType>
	FTask Any(const TaskCollectionType& Tasks)
	{
		if (Tasks.Num() == 0)
		{
			return FTask{};
		}

		struct FSharedData
		{
			explicit FSharedData(u32 InitRefCount, const char* DebugName)
				: RefCount(InitRefCount)
				, Event(DebugName)
			{}

			FTaskEvent Event;
			std::atomic<u32> RefCount;
		};

		FSharedData* SharedData = new FSharedData(Tasks.Num(), "Any_Event");
		FTaskEvent Result = SharedData->Event;
		const i32 Num = Tasks.Num();

		for (const auto& Task : Tasks)
		{
			Launch(
				"Any_Helper",
				[SharedData, Num]()
				{
					FTaskEvent Event = SharedData->Event;
					u32 PrevRefCount = SharedData->RefCount.fetch_sub(1, std::memory_order_acq_rel);

					if (PrevRefCount == static_cast<u32>(Num))
					{
						Event.Trigger();
					}

					if (PrevRefCount == 1)
					{
						delete SharedData;
					}
				},
				Prerequisites(Task),
				ETaskPriority::Default,
				EExtendedTaskPriority::Inline
			);
		}

		return Result;
	}

	// ============================================================================
	// AddNested
	// ============================================================================

	/**
	 * @brief Add a nested task to the currently executing task
	 */
	template<typename TaskType>
	void AddNested(const TaskType& Nested)
	{
		Private::FTaskBase* Parent = Private::GetCurrentTask();
		OLO_CORE_ASSERT(Parent != nullptr, "AddNested must be called from within an executing task");
		Parent->AddNested(*Nested.Pimpl);
	}

	// ============================================================================
	// MakeCompletedTask
	// ============================================================================

	/**
	 * @brief Create an already-completed task with a result value
	 */
	template<typename ResultType, typename... ArgTypes>
	TTask<ResultType> MakeCompletedTask(ArgTypes&&... Args)
	{
		return Launch(
			"CompletedTask",
			[&]() { return ResultType(Forward<ArgTypes>(Args)...); },
			ETaskPriority::Default,
			EExtendedTaskPriority::Inline
		);
	}

	/**
	 * @brief Create an already-completed void task
	 */
	inline TTask<void> MakeCompletedTask()
	{
		return Launch(
			"CompletedTask",
			[]() {},
			ETaskPriority::Default,
			EExtendedTaskPriority::Inline
		);
	}

	// ============================================================================
	// WaitAll
	// ============================================================================

	/**
	 * @brief Wait for all tasks in a collection to complete
	 */
	template<typename TaskCollectionType>
	void WaitAll(const TaskCollectionType& Tasks)
	{
		Wait(Tasks, FMonotonicTimeSpan::Infinity());
	}

	// ============================================================================
	// FTaskPriorityCVar - Runtime configurable task priority
	// ============================================================================

	/**
	 * @class FTaskPriorityCVar
	 * @brief Console variable for configuring task priorities at runtime
	 * 
	 * This class mirrors UE5.7's FTaskPriorityCVar, allowing task priorities to be
	 * configured via console variables. The console variable accepts a string
	 * in the format "[TaskPriority] [ExtendedTaskPriority]".
	 * 
	 * Example usage:
	 * @code
	 * FTaskPriorityCVar CVar{ "r.MyFeature.Priority", "Help text", ETaskPriority::Normal, EExtendedTaskPriority::None };
	 * Launch("MyTask", [] {}, CVar.GetTaskPriority(), CVar.GetExtendedTaskPriority()).Wait();
	 * @endcode
	 */
	class FTaskPriorityCVar
	{
	public:
		/**
		 * @brief Construct a task priority console variable
		 * 
		 * @param Name The console variable name (e.g., "r.Feature.Priority")
		 * @param Help Help text describing what this priority controls
		 * @param DefaultPriority Default task priority
		 * @param DefaultExtendedPriority Default extended priority
		 */
		FTaskPriorityCVar(const char* Name, const char* Help, 
			ETaskPriority DefaultPriority, EExtendedTaskPriority DefaultExtendedPriority);

		/**
		 * @brief Get the current task priority
		 */
		ETaskPriority GetTaskPriority() const
		{
			return m_Priority;
		}

		/**
		 * @brief Get the current extended task priority
		 */
		EExtendedTaskPriority GetExtendedTaskPriority() const
		{
			return m_ExtendedPriority;
		}

	private:
		ETaskPriority m_Priority;
		EExtendedTaskPriority m_ExtendedPriority;
		const char* m_Name;
		const char* m_Help;
	};

} // namespace OloEngine::Tasks
