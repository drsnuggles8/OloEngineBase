#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Thread.h"
#include "Task.h"

#include <atomic>
#include <string>
#include <queue>
#include <mutex>

namespace OloEngine {

	/// <summary>
	/// TaskPipe provides serialized execution of tasks on a dedicated thread.
	/// Useful for systems that require strict ordering (audio, render commands, etc.)
	/// Tasks submitted to a pipe are executed in FIFO order on the pipe's thread.
	/// </summary>
	class TaskPipe : public RefCounted
	{
	public:
		/// <summary>
		/// Create a new task pipe with a dedicated thread
		/// </summary>
		/// <param name="name">Name for the pipe and its thread (for debugging/profiling)</param>
		explicit TaskPipe(const std::string& name);
		
		/// <summary>
		/// Destructor - stops the pipe thread and waits for completion
		/// </summary>
		~TaskPipe();

		// Disable copy semantics
		TaskPipe(const TaskPipe&) = delete;
		TaskPipe& operator=(const TaskPipe&) = delete;

		/// <summary>
		/// Launch a task on this pipe. Tasks execute in FIFO order.
		/// </summary>
		/// <param name="debugName">Name for debugging/profiling</param>
		/// <param name="func">Function to execute</param>
		/// <returns>Task handle for waiting/tracking</returns>
		Ref<Task> Launch(const char* debugName, std::function<void()>&& func);

		/// <summary>
		/// Check if the current thread is this pipe's thread
		/// </summary>
		/// <returns>True if called from pipe thread</returns>
		bool IsOnPipeThread() const;

		/// <summary>
		/// Get the thread ID of this pipe's thread
		/// </summary>
		std::thread::id GetThreadID() const;

		/// <summary>
		/// Get the name of this pipe
		/// </summary>
		const std::string& GetName() const { return m_Name; }

		/// <summary>
		/// Check if the pipe is running
		/// </summary>
		bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

	private:
		void ThreadMain();
		void ExecuteTask(Ref<Task> task);

		std::string m_Name;
		Thread m_Thread;
		ThreadSignal m_WakeEvent;
		
		// Simple FIFO queue with mutex protection (no work stealing)
		std::queue<Ref<Task>> m_Queue;
		std::mutex m_QueueMutex;
		
		std::atomic<bool> m_ShouldExit{false};
		std::atomic<bool> m_Running{false};
	};

} // namespace OloEngine
