#pragma once

#include "OloEngine/Core/Base.h"
#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>
#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <memory>

namespace OloEngine::Audio
{
	//==============================================================================
	/// Function callback wrapper for audio thread tasks
	/// Provides task identification and execution tracking
	class AudioThreadTask
	{
	public:
		using CallbackFunction = std::function<void()>;

		AudioThreadTask(CallbackFunction&& func, const std::string& taskID = "UNNAMED")
			: m_Function(std::move(func)), m_TaskID(taskID)
		{
		}

		void Execute()
		{
			if (m_Function)
			{
				m_Function();
			}
		}

		const std::string& GetTaskID() const { return m_TaskID; }

	private:
		CallbackFunction m_Function;
		std::string m_TaskID;
	};

	//==============================================================================
	/// Reference counter for audio thread synchronization
	/// Allows main thread to wait for audio thread task completion
	class AudioThreadFence
	{
	public:
		AudioThreadFence();
		~AudioThreadFence();

		/// Begin a fence operation - increments counter and schedules decrement on audio thread
		void Begin();

		/// Begin a fence and immediately wait for completion
		void BeginAndWait();

		/// Wait for the current fence to complete
		void Wait() const;

		/// Check if the fence is ready (counter is zero)
		bool IsReady() const { return m_Counter->GetCount() == 0; }

	private:
		class RefCounter
		{
		public:
			void IncRefCount() { m_Count.fetch_add(1, std::memory_order_acq_rel); }
			void DecRefCount() { m_Count.fetch_sub(1, std::memory_order_acq_rel); }
			i32 GetCount() const { return m_Count.load(std::memory_order_acquire); }

		private:
			std::atomic<i32> m_Count{ 0 };
		};

		RefCounter* m_Counter = new RefCounter();
	};

	//==============================================================================
	/// Audio Thread Management System
	/// Provides lock-free task execution on a dedicated audio thread
	/// Based on Hazel's AudioThread architecture
	struct AudioThreadStaticInit;
	
	class AudioThread
	{
		friend struct AudioThreadStaticInit;
		
	public:
		using TaskQueue = SingleReaderMultipleWriterFIFO<std::unique_ptr<AudioThreadTask>>;

		/// Start the audio thread
		static bool Start();

		/// Stop the audio thread and wait for completion
		static bool Stop();

		/// Check if the audio thread is currently running
		static bool IsRunning();

		/// Check if the current thread is the audio thread
		static bool IsAudioThread();

		/// Get the audio thread ID
		static std::thread::id GetThreadID();

		/// Add a task to the audio thread queue
		static void AddTask(std::unique_ptr<AudioThreadTask> task);

		/// Execute a function on the audio thread
		static void ExecuteOnAudioThread(std::function<void()> func, const std::string& taskID = "UNNAMED");

		/// Execute a function on audio thread with policy control
		enum class ExecutionPolicy
		{
			ExecuteNow,   // If on audio thread, execute immediately
			ExecuteAsync  // Always add to queue
		};

		static void ExecuteOnAudioThread(ExecutionPolicy policy, std::function<void()> func, const std::string& taskID = "UNNAMED");

		/// Get timing statistics
		static f64 GetLastUpdateTime() { return s_LastUpdateTime; }

	private:
		static void ThreadFunction();
		static void OnUpdate();

		// Thread management
		static std::atomic<bool> s_ThreadActive;
		static std::unique_ptr<std::thread> s_AudioThread;
		static std::thread::id s_AudioThreadID;

		// Task queue
		static TaskQueue s_TaskQueue;
		static constexpr u32 TASK_QUEUE_SIZE = 1024; // Must be power of 2

		// Performance tracking
		static std::atomic<f64> s_LastUpdateTime;
	};

} // namespace OloEngine::Audio