// Pipe.h - Serial task execution pipe
// Ported from UE5.7 Tasks/Pipe.h

#pragma once

#include "OloEngine/Task/TaskPrivate.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/HAL/EventCount.h"
#include "OloEngine/Templates/RefCounting.h"
#include "OloEngine/Containers/Array.h"

#include <atomic>
#include <memory>

namespace OloEngine::Tasks
{
	// @class FPipeCallStack
	// @brief Maintains pipe callstack for nested pipe execution
	// 
	// Due to busy waiting, tasks from multiple pipes can be executed nested.
	// This class tracks the pipe context for IsInContext() checks.
	class FPipeCallStack
	{
	public:
		static void Push(const FPipe& Pipe)
		{
			s_CallStack.Add(&Pipe);
		}

		static void Pop([[maybe_unused]] const FPipe& Pipe)
		{
			OLO_CORE_ASSERT(s_CallStack.Num() > 0 && s_CallStack.Last() == &Pipe, 
				"Pipe call stack mismatch");
			s_CallStack.Pop(EAllowShrinking::No);
		}

		// @brief Check if a task from the given pipe is on top of the call stack
		// 
		// Only checks the top because even if the pipe is deeper in the stack,
		// logically it's a bug to assume it's safe (accidental condition).
		static bool IsOnTop(const FPipe& Pipe)
		{
			return s_CallStack.Num() > 0 && s_CallStack.Last() == &Pipe;
		}

	private:
		static inline thread_local TArray<const FPipe*> s_CallStack;
	};

	// @class FPipe
	// @brief A chain of tasks that are executed one after another
	// 
	// FPipe guarantees non-concurrent task execution, making it useful for
	// synchronizing access to a shared resource. It's a lightweight alternative
	// to dedicated threads when you need serial execution.
	// 
	// Key properties:
	// - Execution order is FIFO for tasks without prerequisites
	// - A pipe must be alive until its last task is completed
	// - Uses prerequisite system instead of blocking waits
	// 
	// Example:
	// @code
	// FPipe ResourcePipe("ResourcePipe");
	// 
	// // These tasks will execute serially, never concurrently
	// ResourcePipe.Launch("ReadResource", [&resource]() { return resource.Read(); });
	// ResourcePipe.Launch("WriteResource", [&resource]() { resource.Write(42); });
	// 
	// ResourcePipe.WaitUntilEmpty();
	// @endcode
	class FPipe
	{
	public:
		OLO_NONCOPYABLE(FPipe);

		// @brief Construct a pipe with a debug name
		explicit FPipe(const char* InDebugName)
			: m_EmptyEventRef(std::make_shared<FEventCount>())
			, m_DebugName(InDebugName)
		{
		}

		~FPipe()
		{
			OLO_CORE_ASSERT(!HasWork(), "FPipe destroyed with pending tasks");
		}

		// @brief Check if the pipe has any incomplete tasks
		bool HasWork() const
		{
			return m_TaskCount.load(std::memory_order_relaxed) != 0;
		}

		// @brief Wait until the pipe is empty (all tasks completed)
		bool WaitUntilEmpty(FMonotonicTimeSpan InTimeout = FMonotonicTimeSpan::Infinity())
		{
			if (m_TaskCount.load(std::memory_order_acquire) == 0)
			{
				return true;
			}

			OLO_PROFILE_SCOPE("FPipe::WaitUntilEmpty");

			FTimeout Timeout(InTimeout);
			while (true)
			{
				if (m_TaskCount.load(std::memory_order_acquire) == 0)
				{
					return true;
				}

				if (Timeout.IsExpired())
				{
					return false;
				}

				auto Token = m_EmptyEventRef->PrepareWait();

				if (m_TaskCount.load(std::memory_order_acquire) == 0)
				{
					return true;
				}

				FMonotonicTimeSpan WaitTime = Timeout.WillNeverExpire() ? 
					FMonotonicTimeSpan::Infinity() : 
					Timeout.GetRemainingTime();

				if (!m_EmptyEventRef->WaitFor(Token, WaitTime))
				{
					break;
				}
			}

			return false;
		}

		// @brief Launch a task in the pipe for serial execution
		// 
		// Tasks launched in the same pipe are guaranteed to execute serially
		// in FIFO order. They will never run concurrently with each other.
		// 
		// Uses the prerequisite system - no blocking waits inside tasks.
		template<typename TaskBodyType>
		TTask<TInvokeResult_T<TaskBodyType>> Launch(
			const char* InDebugName,
			TaskBodyType&& TaskBody,
			ETaskPriority Priority = ETaskPriority::Default,
			EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
			ETaskFlags Flags = ETaskFlags::None
		)
		{
			using FResult = TInvokeResult_T<TaskBodyType>;
			using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;

			FExecutableTask* Task = FExecutableTask::Create(InDebugName, Forward<TaskBodyType>(TaskBody),
				Priority, ExtendedPriority, Flags);
			
			m_TaskCount.fetch_add(1, std::memory_order_acq_rel);
			Task->SetPipe(*this);
			Task->TryLaunch(sizeof(*Task));
			
			return TTask<FResult>{ Task };
		}

		// @brief Launch a task with prerequisites in the pipe
		template<typename TaskBodyType, typename PrerequisitesCollectionType>
		TTask<TInvokeResult_T<TaskBodyType>> Launch(
			const char* InDebugName,
			TaskBodyType&& TaskBody,
			PrerequisitesCollectionType&& Prerequisites,
			ETaskPriority Priority = ETaskPriority::Default,
			EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None,
			ETaskFlags Flags = ETaskFlags::None
		)
		{
			using FResult = TInvokeResult_T<TaskBodyType>;
			using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;

			FExecutableTask* Task = FExecutableTask::Create(InDebugName, Forward<TaskBodyType>(TaskBody),
				Priority, ExtendedPriority, Flags);
			
			m_TaskCount.fetch_add(1, std::memory_order_acq_rel);
			
			// Order matters: pipe must be set before prerequisites try to unlock
			Task->SetPipe(*this);
			Task->AddPrerequisites(Forward<PrerequisitesCollectionType>(Prerequisites));
			Task->TryLaunch(sizeof(*Task));
			
			return TTask<FResult>{ Task };
		}

		// @brief Check if a task from this pipe is currently executing on this thread
		bool IsInContext() const
		{
			return FPipeCallStack::IsOnTop(*this);
		}

		const char* GetDebugName() const
		{
			return m_DebugName;
		}

	private:
		friend class Private::FTaskBase;

		// @brief Push a task into the pipe
		// 
		// Adds the task as subsequent to the last task and sets it as new last.
		// @return The previous last task (caller must release), or nullptr if pipe was empty
		[[nodiscard]] Private::FTaskBase* PushIntoPipe(Private::FTaskBase& Task)
		{
			Task.AddRef(); // Pipe holds ref to last task
			
			Private::FTaskBase* LastTask_Local = m_LastTask.exchange(&Task, std::memory_order_acq_rel);
			OLO_CORE_ASSERT(LastTask_Local != &Task, 
				"Dependency cycle: adding itself as a prerequisite");

			if (LastTask_Local == nullptr)
			{
				return nullptr;
			}

			if (!LastTask_Local->AddSubsequent(Task))
			{
				// Last task already completed
				LastTask_Local->Release();
				return nullptr;
			}

			return LastTask_Local; // Transfer reference to caller
		}

		// @brief Clear a task from the pipe
		// 
		// Called when task execution finishes, before completion.
		void ClearTask(Private::FTaskBase& Task)
		{
			Private::FTaskBase* Task_Local = &Task;
			
			// Try clearing if still the last task
			if (m_LastTask.compare_exchange_strong(Task_Local, nullptr, 
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				Task.Release();
			}

			// Take ref on event before decrementing to avoid use-after-free
			std::shared_ptr<FEventCount> LocalEmptyEvent = m_EmptyEventRef;
			if (m_TaskCount.fetch_sub(1, std::memory_order_release) == 1)
			{
				LocalEmptyEvent->Notify();
			}
		}

		void ExecutionStarted()
		{
			FPipeCallStack::Push(*this);
		}

		void ExecutionFinished()
		{
			FPipeCallStack::Pop(*this);
		}

	private:
		std::atomic<Private::FTaskBase*> m_LastTask{ nullptr };
		std::atomic<u64> m_TaskCount{ 0 };
		std::shared_ptr<FEventCount> m_EmptyEventRef;
		const char* const m_DebugName;
	};

	// ============================================================================
	// FTaskBase pipe-related method implementations
	// ============================================================================

	namespace Private
	{
		inline FTaskBase* FTaskBase::TryPushIntoPipe()
		{
			return m_Pipe->PushIntoPipe(*this);
		}

		inline void FTaskBase::ClearPipe()
		{
			m_Pipe->ClearTask(*this);
		}

		inline void FTaskBase::StartPipeExecution()
		{
			m_Pipe->ExecutionStarted();
		}

		inline void FTaskBase::FinishPipeExecution()
		{
			m_Pipe->ExecutionFinished();
		}
	}

} // namespace OloEngine::Tasks
