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
    
    std::queue<std::unique_ptr<AudioThread::CompletionToken>> AudioThread::s_TaskQueue{};
    std::mutex AudioThread::s_TaskQueueMutex{};
    std::mutex AudioThread::s_StartStopMutex{};
    std::condition_variable AudioThread::s_TaskCondition{};
    std::condition_variable AudioThread::s_CompletionCondition{};
    std::atomic<int> AudioThread::s_PendingTasks{ 0 };

    bool AudioThread::Start()
    {
        // Serialize start operations to prevent race conditions
        std::lock_guard<std::mutex> startLock(s_StartStopMutex);
        
        // Use atomic compare-exchange to ensure only one thread can transition to running
        bool expected = false;
        if (!s_IsRunning.compare_exchange_strong(expected, true))
        {
            OLO_CORE_WARN("AudioThread is already running");
            return false;
        }

        s_ShouldStop.store(false);
        
        try
        {
            s_AudioThread = std::make_unique<std::thread>(AudioThreadLoop);
            s_AudioThreadID = s_AudioThread->get_id();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to create AudioThread: {}", e.what());
            s_IsRunning.store(false);
            return false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("Failed to create AudioThread: unknown exception");
            s_IsRunning.store(false);
            return false;
        }
        
        // Wait for thread to start (thread loop will confirm it's running)
        std::unique_lock<std::mutex> lock(s_TaskQueueMutex);
        s_TaskCondition.wait(lock, [] { return s_IsRunning.load(); });
        
        OLO_CORE_INFO("AudioThread started with ID: {}", (void*)&s_AudioThreadID);
        return true;
    }

    void AudioThread::Stop()
    {
        // Serialize stop operations with start operations
        std::lock_guard<std::mutex> startLock(s_StartStopMutex);
        
        if (!s_IsRunning.load())
        {
            OLO_CORE_WARN("AudioThread is not running");
            return;
        }

        s_ShouldStop.store(true);
        s_TaskCondition.notify_all();

        // Check if we're trying to stop from within the audio thread itself
        if (std::this_thread::get_id() == s_AudioThreadID)
        {
            // We're on the audio thread - cannot join ourselves
            OLO_CORE_WARN("AudioThread::Stop() called from within audio thread - performing local cleanup only");
            
            // Clear any remaining tasks
            {
                std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
                while (!s_TaskQueue.empty())
                {
                    s_TaskQueue.pop();
                }
                s_PendingTasks.store(0);
                // Notify any threads waiting for task completion
                s_CompletionCondition.notify_all();
            }
            
            // Note: s_IsRunning, thread join, and reset will be handled by the thread loop exit
            // or by an external thread calling Stop() again
            return;
        }

        // Normal case: called from a different thread
        if (s_AudioThread && s_AudioThread->joinable())
        {
            s_AudioThread->join();
        }

        s_AudioThread.reset();
        s_AudioThreadID = std::thread::id{};
        
        // Only clear s_IsRunning after thread has been joined
        s_IsRunning.store(false);
        
        // Clear any remaining tasks
        {
            std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
            while (!s_TaskQueue.empty())
            {
                s_TaskQueue.pop();
            }
            s_PendingTasks.store(0);
            // Notify any threads waiting for task completion
            s_CompletionCondition.notify_all();
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

    std::future<void> AudioThread::ExecuteOnAudioThread(Task task, bool waitForCompletion)
    {
        if (!task)
        {
            OLO_CORE_ERROR("Cannot execute null task on AudioThread");
            // Return a future that is immediately ready with an exception
            std::promise<void> promise;
            auto future = promise.get_future();
            promise.set_exception(std::make_exception_ptr(std::invalid_argument("Null task")));
            return future;
        }

        if (!s_IsRunning.load())
        {
            OLO_CORE_ERROR("Cannot execute task: AudioThread is not running");
            // Return a future that is immediately ready with an exception
            std::promise<void> promise;
            auto future = promise.get_future();
            promise.set_exception(std::make_exception_ptr(std::runtime_error("AudioThread not running")));
            return future;
        }

        // If we're already on the audio thread, execute immediately
        if (IsAudioThread())
        {
            std::promise<void> promise;
            auto future = promise.get_future();
            try
            {
                task();
                promise.set_value();
            }
            catch (...)
            {
                promise.set_exception(std::current_exception());
            }
            return future;
        }

        // Create completion token
        auto token = std::make_unique<CompletionToken>(std::move(task));
        auto future = token->promise.get_future();

        // Add task to queue
        {
            std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
            s_TaskQueue.push(std::move(token));
            s_PendingTasks.fetch_add(1);
        }
        
        s_TaskCondition.notify_one();

        // Wait for completion if requested
        if (waitForCompletion)
        {
            future.wait();
        }

        return future;
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
        
        // Thread is exiting - perform final cleanup
        // This handles the case where Stop() was called from within this thread
        s_IsRunning.store(false);
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
            auto token = std::move(s_TaskQueue.front());
            s_TaskQueue.pop();
            
            // Execute task without holding the lock
            lock.unlock();
            
            try
            {
                token->task();
                token->promise.set_value();
            }
            catch (...)
            {
                token->promise.set_exception(std::current_exception());
            }
            
            s_PendingTasks.fetch_sub(1);
            lock.lock();
        }
        
        // Notify completion
        s_CompletionCondition.notify_all();
    }

} // namespace OloEngine::Audio