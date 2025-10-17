#include "OloEnginePCH.h"
#include "TaskPipe.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	TaskPipe::TaskPipe(const std::string& name)
		: m_Name(name)
		, m_Thread(name)
		, m_WakeFlag(false)  // C++20 atomic for wait/notify
	{
		// Start the pipe thread
		m_Running.store(true, std::memory_order_release);
		m_Thread.Dispatch([this]() { ThreadMain(); });
	}

	TaskPipe::~TaskPipe()
	{
		// Signal thread to exit
		m_ShouldExit.store(true, std::memory_order_release);
		
		// Signal thread multiple times to ensure it catches the signal
		// C++20 atomic notify is more reliable than event signaling
		for (int i = 0; i < 3; ++i)
		{
			m_WakeFlag.store(true, std::memory_order_release);
			m_WakeFlag.notify_one();
			std::this_thread::yield();
		}
		
		// Wait for thread to finish
		m_Thread.Join();
		m_Running.store(false, std::memory_order_release);
	}

	Ref<Task> TaskPipe::Launch(const char* debugName, std::function<void()>&& func)
	{
		// Create task
		Ref<Task> task = CreateTask(debugName, ETaskPriority::Normal, std::move(func));
		
		// Transition to Scheduled state
		ETaskState expected = ETaskState::Ready;
		if (!task->TryTransitionState(expected, ETaskState::Scheduled))
		{
			// Task already scheduled/running/completed - shouldn't happen
			return task;
		}
		
		// Push to queue (thread-safe)
		{
			std::lock_guard<std::mutex> lock(m_QueueMutex);
			m_Queue.push(task);
		}
		
		// Signal the pipe thread using C++20 atomic notify
		m_WakeFlag.store(true, std::memory_order_release);
		m_WakeFlag.notify_one();
		
		return task;
	}

	bool TaskPipe::IsOnPipeThread() const
	{
		return std::this_thread::get_id() == m_Thread.GetID();
	}

	std::thread::id TaskPipe::GetThreadID() const
	{
		return m_Thread.GetID();
	}

	void TaskPipe::ThreadMain()
	{
		while (!m_ShouldExit.load(std::memory_order_acquire))
		{
			Ref<Task> task;
			
			// Try to get a task from the queue
			{
				std::lock_guard<std::mutex> lock(m_QueueMutex);
				if (!m_Queue.empty())
				{
					task = m_Queue.front();
					m_Queue.pop();
				}
			}
			
			if (task)
			{
				// Execute task
				ExecuteTask(task);
			}
			else
			{
				// No work - wait using C++20 atomic wait
				// Check exit flag before waiting
				if (m_ShouldExit.load(std::memory_order_acquire))
					break;
				
				// Wait for wake flag to become true
				m_WakeFlag.wait(false, std::memory_order_acquire);
				
				// Reset the wake flag for next wait
				m_WakeFlag.store(false, std::memory_order_relaxed);
			}
		}
		
		// Process remaining tasks in queue before exiting
		while (true)
		{
			Ref<Task> task;
			{
				std::lock_guard<std::mutex> lock(m_QueueMutex);
				if (m_Queue.empty())
					break;
				
				task = m_Queue.front();
				m_Queue.pop();
			}
			
			ExecuteTask(task);
		}
	}

	void TaskPipe::ExecuteTask(Ref<Task> task)
	{
		// Always execute tasks that are in the queue, even during shutdown
		// The ThreadMain loop already processed the queue drain during shutdown
		
		// Transition to Running state
		ETaskState expected = ETaskState::Scheduled;
		if (!task->TryTransitionState(expected, ETaskState::Running))
		{
			// Task already running or completed
			return;
		}
		
		// Execute the task
		try
		{
			task->Execute();
		}
		catch (...)
		{
			// Suppress exceptions - don't let them crash the pipe thread
		}
		
		// Transition to Completed state
		task->SetState(ETaskState::Completed);
		
		// Notify any dependent tasks
		task->OnCompleted();
	}

} // namespace OloEngine
