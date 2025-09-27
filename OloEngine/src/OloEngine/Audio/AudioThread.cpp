#include "OloEnginePCH.h"
#include "AudioThread.h"

#include <chrono>

namespace OloEngine::Audio
{
    // Static member definitions
    std::unique_ptr<std::thread> AudioThread::s_AudioThread = nullptr;
    std::atomic<bool> AudioThread::s_ShouldStop{ false };
    std::atomic<bool> AudioThread::s_IsRunning{ false };
    std::thread::id AudioThread::s_AudioThreadID{};
    
    std::queue<AudioThread::Task> AudioThread::s_TaskQueue{};
    std::mutex AudioThread::s_TaskQueueMutex{};
    std::condition_variable AudioThread::s_TaskCondition{};
    std::condition_variable AudioThread::s_CompletionCondition{};
    std::atomic<int> AudioThread::s_PendingTasks{ 0 };

    bool AudioThread::Start()
    {
        if (s_IsRunning.load())
        {
            OLO_CORE_WARN("AudioThread is already running");
            return false;
        }

        s_ShouldStop.store(false);
        s_AudioThread = std::make_unique<std::thread>(AudioThreadLoop);
        
        // Wait for thread to start
        std::unique_lock<std::mutex> lock(s_TaskQueueMutex);
        s_TaskCondition.wait(lock, [] { return s_IsRunning.load(); });
        
        OLO_CORE_INFO("AudioThread started with ID: {}", (void*)&s_AudioThreadID);
        return true;
    }

    void AudioThread::Stop()
    {
        if (!s_IsRunning.load())
        {
            OLO_CORE_WARN("AudioThread is not running");
            return;
        }

        s_ShouldStop.store(true);
        s_TaskCondition.notify_all();

        if (s_AudioThread && s_AudioThread->joinable())
        {
            s_AudioThread->join();
        }

        s_AudioThread.reset();
        s_IsRunning.store(false);
        
        // Clear any remaining tasks
        {
            std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
            while (!s_TaskQueue.empty())
            {
                s_TaskQueue.pop();
            }
            s_PendingTasks.store(0);
        }

        OLO_CORE_INFO("AudioThread stopped");
    }

    bool AudioThread::IsRunning()
    {
        return s_IsRunning.load();
    }

    bool AudioThread::IsAudioThread()
    {
        return std::this_thread::get_id() == s_AudioThreadID;
    }

    std::thread::id AudioThread::GetThreadID()
    {
        return s_AudioThreadID;
    }

    void AudioThread::ExecuteOnAudioThread(Task task, bool waitForCompletion)
    {
        if (!task)
        {
            OLO_CORE_ERROR("Cannot execute null task on AudioThread");
            return;
        }

        if (!s_IsRunning.load())
        {
            OLO_CORE_ERROR("Cannot execute task: AudioThread is not running");
            return;
        }

        // If we're already on the audio thread, execute immediately
        if (IsAudioThread())
        {
            task();
            return;
        }

        // Add task to queue
        {
            std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
            s_TaskQueue.push(task);
            s_PendingTasks.fetch_add(1);
        }
        
        s_TaskCondition.notify_one();

        // Wait for completion if requested
        if (waitForCompletion)
        {
            std::unique_lock<std::mutex> lock(s_TaskQueueMutex);
            s_CompletionCondition.wait(lock, [&task] { 
                // This is a simplified completion check
                // In a real implementation, you'd want to track specific task completion
                return s_PendingTasks.load() == 0;
            });
        }
    }

    size_t AudioThread::GetPendingTaskCount()
    {
        return static_cast<size_t>(s_PendingTasks.load());
    }

    void AudioThread::AudioThreadLoop()
    {
        s_AudioThreadID = std::this_thread::get_id();
        s_IsRunning.store(true);
        
        // Set thread priority (platform-specific)
        #ifdef OLO_PLATFORM_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        #endif
        
        // Notify that thread has started
        s_TaskCondition.notify_all();
        
        OLO_CORE_INFO("AudioThread loop started");

        while (!s_ShouldStop.load())
        {
            ProcessTasks();
            
            // Small sleep to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        OLO_CORE_INFO("AudioThread loop ended");
    }

    void AudioThread::ProcessTasks()
    {
        std::unique_lock<std::mutex> lock(s_TaskQueueMutex);
        
        // Wait for tasks or stop signal
        s_TaskCondition.wait_for(lock, std::chrono::milliseconds(1), 
            [] { return !s_TaskQueue.empty() || s_ShouldStop.load(); });

        // Process all available tasks
        while (!s_TaskQueue.empty() && !s_ShouldStop.load())
        {
            Task task = std::move(s_TaskQueue.front());
            s_TaskQueue.pop();
            
            // Execute task without holding the lock
            lock.unlock();
            
            try
            {
                task();
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("AudioThread task threw exception: {}", e.what());
            }
            catch (...)
            {
                OLO_CORE_ERROR("AudioThread task threw unknown exception");
            }
            
            s_PendingTasks.fetch_sub(1);
            lock.lock();
        }
        
        // Notify completion
        s_CompletionCondition.notify_all();
    }

} // namespace OloEngine::Audio