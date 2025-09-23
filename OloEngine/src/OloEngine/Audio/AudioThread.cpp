#include "OloEnginePCH.h"
#include "AudioThread.h"

namespace OloEngine::Audio
{
	//==============================================================================
	// AudioThread Static Members
	//==============================================================================
	std::atomic<bool> AudioThread::s_ThreadActive{ false };
	std::unique_ptr<std::thread> AudioThread::s_AudioThread = nullptr;
	std::thread::id AudioThread::s_AudioThreadID{};
	AudioThread::TaskQueue AudioThread::s_TaskQueue;
	std::atomic<f64> AudioThread::s_LastUpdateTime{ 0.0 };

	// Static initialization helper
	struct AudioThreadStaticInit
	{
		AudioThreadStaticInit()
		{
			AudioThread::s_TaskQueue.reset(AudioThread::TASK_QUEUE_SIZE);
		}
	};
	static AudioThreadStaticInit s_StaticInit;

	//==============================================================================
	// AudioThreadFence Implementation
	//==============================================================================
	AudioThreadFence::AudioThreadFence()
	{
		OLO_CORE_ASSERT(!AudioThread::IsAudioThread(), "AudioThreadFence cannot be created on audio thread");
	}

	AudioThreadFence::~AudioThreadFence()
	{
		OLO_CORE_ASSERT(!AudioThread::IsAudioThread(), "AudioThreadFence cannot be destroyed on audio thread");
		Wait();
		delete m_Counter;
	}

	void AudioThreadFence::Begin()
	{
		OLO_CORE_ASSERT(!AudioThread::IsAudioThread(), "AudioThreadFence::Begin() called from audio thread");

		if (!AudioThread::IsRunning())
			return;

		Wait();

		m_Counter->IncRefCount();

		AudioThread::ExecuteOnAudioThread([counter = m_Counter]()
		{
			counter->DecRefCount();
		}, "AudioThreadFence");
	}

	void AudioThreadFence::BeginAndWait()
	{
		Begin();
		Wait();
	}

	void AudioThreadFence::Wait() const
	{
		OLO_CORE_ASSERT(!AudioThread::IsAudioThread(), "AudioThreadFence::Wait() called from audio thread");

		while (!IsReady())
		{
			std::this_thread::yield();
		}
	}

	//==============================================================================
	// AudioThread Implementation
	//==============================================================================
	bool AudioThread::Start()
	{
		if (s_ThreadActive.load())
			return false;

		s_ThreadActive.store(true);
		s_AudioThread = std::make_unique<std::thread>(ThreadFunction);
		s_AudioThreadID = s_AudioThread->get_id();

		// Remove logging to avoid potential issues
		// OLO_CORE_INFO("Audio Thread started with ID: {}", std::hash<std::thread::id>{}(s_AudioThreadID));
		return true;
	}

	bool AudioThread::Stop()
	{
		if (!s_ThreadActive.load())
			return false;

		s_ThreadActive.store(false);

		if (s_AudioThread && s_AudioThread->joinable())
		{
			s_AudioThread->join();
		}

		s_AudioThread.reset();
		// Remove logging to avoid potential issues
		// OLO_CORE_INFO("Audio Thread stopped");
		return true;
	}

	bool AudioThread::IsRunning()
	{
		return s_ThreadActive.load();
	}

	bool AudioThread::IsAudioThread()
	{
		return std::this_thread::get_id() == s_AudioThreadID;
	}

	std::thread::id AudioThread::GetThreadID()
	{
		return s_AudioThreadID;
	}

	void AudioThread::AddTask(std::unique_ptr<AudioThreadTask> task)
	{
		if (!s_TaskQueue.push(std::move(task)))
		{
			OLO_CORE_WARN("Audio thread task queue is full! Task '{}' dropped", task ? task->GetTaskID() : "INVALID");
		}
	}

	void AudioThread::ExecuteOnAudioThread(std::function<void()> func, const std::string& taskID)
	{
		auto task = std::make_unique<AudioThreadTask>(std::move(func), taskID);
		AddTask(std::move(task));
	}

	void AudioThread::ExecuteOnAudioThread(ExecutionPolicy policy, std::function<void()> func, const std::string& taskID)
	{
		if (IsAudioThread())
		{
			switch (policy)
			{
			case ExecutionPolicy::ExecuteNow:
				func();
				break;
			case ExecutionPolicy::ExecuteAsync:
			default:
				ExecuteOnAudioThread(std::move(func), taskID);
				break;
			}
		}
		else
		{
			ExecuteOnAudioThread(std::move(func), taskID);
		}
	}

	void AudioThread::ThreadFunction()
	{
		// Remove logging to avoid potential issues
		// OLO_CORE_INFO("Audio Thread started execution");

#ifdef OLO_PLATFORM_WINDOWS
		// Set thread name for debugging
		SetThreadDescription(GetCurrentThread(), L"OloEngine Audio Thread");
#endif

		while (s_ThreadActive.load())
		{
			OnUpdate();
			
			// Small sleep to prevent 100% CPU usage
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}

		// Remove logging to avoid potential issues
		// OLO_CORE_INFO("Audio Thread finished execution");
	}

	void AudioThread::OnUpdate()
	{
		// Process all available tasks
		std::unique_ptr<AudioThreadTask> task;
		u32 tasksProcessed = 0;
		auto start = std::chrono::steady_clock::now();

		while (s_TaskQueue.pop(task) && task)
		{
			try
			{
				task->Execute();
				++tasksProcessed;
			}
			catch (const std::exception& e)
			{
				// Remove logging to avoid potential issues
				// OLO_CORE_ERROR("Audio thread task '{}' threw exception: {}", task->GetTaskID(), e.what());
			}
			catch (...)
			{
				// Remove logging to avoid potential issues
				// OLO_CORE_ERROR("Audio thread task '{}' threw unknown exception", task->GetTaskID());
			}

			// Reset task pointer
			task.reset();
		}

		auto elapsed = std::chrono::steady_clock::now() - start;
		auto elapsedMs = std::chrono::duration<double, std::milli>(elapsed).count();
		s_LastUpdateTime.store(elapsedMs);

		// Log performance warning if update took too long (removed for debugging)
		// if (elapsedMs > 1.0) // More than 1ms
		// {
		//     OLO_CORE_WARN("Audio thread update took {:.2f}ms (processed {} tasks)", elapsedMs, tasksProcessed);
		// }
	}

} // namespace OloEngine::Audio