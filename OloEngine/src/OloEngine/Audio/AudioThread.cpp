#include "OloEnginePCH.h"
#include "AudioThread.h"

#include <chrono>

namespace OloEngine::Audio
{
    // Helper function to create a ready future
    namespace
    {
        std::future<void> MakeReadyFuture()
        {
            std::promise<void> promise;
            promise.set_value();
            return promise.get_future();
        }
    }

    // Thread-local flag for lock-free IsAudioThread() check
    // Set to true when the audio thread starts, false otherwise
    thread_local bool t_IsAudioThread = false;
    
    // Static member definitions
    std::unique_ptr<std::thread> AudioThread::s_AudioThread = nullptr;
    std::atomic<bool> AudioThread::s_ShouldStop{ false };
    std::atomic<bool> AudioThread::s_IsRunning{ false };
    std::atomic<bool> AudioThread::s_IsInitialized{ false };
    std::atomic<std::thread::id> AudioThread::s_AudioThreadID{};
    
    std::queue<std::unique_ptr<AudioThread::CompletionToken>> AudioThread::s_TaskQueue{};
    std::mutex AudioThread::s_TaskQueueMutex{};
    std::mutex AudioThread::s_StartStopMutex{};
    std::condition_variable AudioThread::s_TaskCondition{};
    std::condition_variable AudioThread::s_CompletionCondition{};
    std::atomic<sizet> AudioThread::s_PendingTasks{ 0 };

    bool AudioThread::Start()
    {
        OLO_PROFILE_FUNCTION();

        // Serialize start operations to prevent race conditions
        std::lock_guard<std::mutex> startLock(s_StartStopMutex);
        
        // Use atomic compare-exchange to ensure only one thread can transition to running
        bool expected = false;
        if (!s_IsRunning.compare_exchange_strong(expected, true))
        {
            OLO_CORE_WARN("AudioThread is already running");
            return false;
        }

        // Reset initialization flag before starting thread
        s_IsInitialized.store(false, std::memory_order_release);
        s_ShouldStop.store(false);
        
        try
        {
            s_AudioThread = std::make_unique<std::thread>(AudioThreadLoop);
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
        
        // Wait for thread initialization to complete (thread loop will signal via s_IsInitialized)
        std::unique_lock<std::mutex> lock(s_TaskQueueMutex);
        s_TaskCondition.wait(lock, [] { return s_IsInitialized.load(); });
        
        OLO_CORE_INFO("AudioThread started with ID: {}", std::hash<std::thread::id>{}(s_AudioThreadID.load(std::memory_order_acquire)));
        return true;
    }

    void AudioThread::Stop()
    {
        OLO_PROFILE_FUNCTION();

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
        if (std::this_thread::get_id() == s_AudioThreadID.load(std::memory_order_acquire))
        {
            // We're on the audio thread - cannot join ourselves
            OLO_CORE_WARN("AudioThread::Stop() called from within audio thread - performing local cleanup only");
            
            // Clear any remaining tasks
            ClearPendingTasks();
            
            // Detach the thread so it can clean up on its own, then reset static state
            if (s_AudioThread && s_AudioThread->joinable())
            {
                s_AudioThread->detach();
            }
            s_AudioThread.reset();
            s_AudioThreadID.store(std::thread::id{}, std::memory_order_release);
            s_IsInitialized.store(false);
            s_IsRunning.store(false);
            
            return;
        }

        // Normal case: called from a different thread
        if (s_AudioThread && s_AudioThread->joinable())
        {
            s_AudioThread->join();
        }

        s_AudioThread.reset();
        s_AudioThreadID.store(std::thread::id{}, std::memory_order_release);
        
        // Clear initialization and running flags after thread has been joined
        s_IsInitialized.store(false);
        s_IsRunning.store(false);
        
        // Clear any remaining tasks
        ClearPendingTasks();

        OLO_CORE_INFO("AudioThread stopped");
    }

    bool AudioThread::IsRunning()
    {
        return s_IsRunning.load();
    }

    bool AudioThread::IsAudioThread()
    {
        // Lock-free check using thread-local storage
        // This flag is set by the audio thread itself, avoiding mutex overhead
        return t_IsAudioThread;
    }

    std::thread::id AudioThread::GetThreadID()
    {
        return s_AudioThreadID.load(std::memory_order_acquire);
    }

    std::future<void> AudioThread::ExecuteOnAudioThread(Task task)
    {
        OLO_PROFILE_FUNCTION();

        if (!task)
        {
            OLO_CORE_ERROR("Cannot execute null task on AudioThread");
            // Return a future that is immediately ready with an exception
            std::promise<void> promise;
            auto future = promise.get_future();
            promise.set_exception(std::make_exception_ptr(std::invalid_argument("Null task")));
            return future;
        }

        // If we're already on the audio thread, execute immediately
        if (IsAudioThread())
        {
            try
            {
                task();
                // Return a pre-completed future to avoid promise allocation overhead
                return MakeReadyFuture();
            }
            catch (...)
            {
                // For exceptions, we still need a promise to propagate the exception
                std::promise<void> promise;
                auto future = promise.get_future();
                promise.set_exception(std::current_exception());
                return future;
            }
        }

        // Create completion token and future before locking
        auto token = std::make_unique<CompletionToken>(std::move(task));
        auto future = token->m_Promise.get_future();

        // Protect both the running check and task enqueue to prevent TOCTOU race with Stop()
        // This ensures Stop() cannot clear pending tasks between our check and enqueue
        {
            std::lock_guard<std::mutex> stateLock(s_StartStopMutex);
            
            if (!s_IsRunning.load())
            {
                OLO_CORE_ERROR("Cannot execute task: AudioThread is not running");
                // Set exception on the promise we already created
                token->m_Promise.set_exception(std::make_exception_ptr(std::runtime_error("AudioThread not running")));
                return future;
            }

            // Add task to queue while still holding state lock
            {
                std::lock_guard<std::mutex> queueLock(s_TaskQueueMutex);
                s_TaskQueue.push(std::move(token));
                s_PendingTasks.fetch_add(1);
            }
        } // Release state lock before notifying
        
        s_TaskCondition.notify_one();

        return future;
    }

    sizet AudioThread::GetPendingTaskCount()
    {
        return s_PendingTasks.load();
    }

    void AudioThread::AudioThreadLoop()
    {
        OLO_PROFILE_FUNCTION();

        // Mark this thread as the audio thread (lock-free for IsAudioThread())
        t_IsAudioThread = true;
        
        s_AudioThreadID.store(std::this_thread::get_id(), std::memory_order_release);
        
        // Set thread priority (platform-specific)
        #ifdef OLO_PLATFORM_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        #endif
        
        // Signal that thread initialization is complete
        s_IsInitialized.store(true);
        s_TaskCondition.notify_all();
        
        OLO_CORE_INFO("AudioThread loop started");

        while (!s_ShouldStop.load())
        {
            ProcessTasks();
        }

        OLO_CORE_INFO("AudioThread loop ended");
        
        // Clear the thread-local flag as we're exiting the audio thread
        t_IsAudioThread = false;
        
        // Thread is exiting - perform final cleanup
        // This handles the case where Stop() was called from within this thread
        s_IsRunning.store(false);
    }

    void AudioThread::ProcessTasks()
    {
        OLO_PROFILE_FUNCTION();
        
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
                token->m_Task();
                token->m_Promise.set_value();
            }
            catch (...)
            {
                token->m_Promise.set_exception(std::current_exception());
            }
            
            s_PendingTasks.fetch_sub(1);
            lock.lock();
        }
        
        // Notify completion
        s_CompletionCondition.notify_all();
    }

    void AudioThread::ClearPendingTasks()
    {
        std::lock_guard<std::mutex> lock(s_TaskQueueMutex);
        while (!s_TaskQueue.empty())
        {
            auto token = std::move(s_TaskQueue.front());
            s_TaskQueue.pop();
            token->m_Promise.set_exception(
                std::make_exception_ptr(std::runtime_error("AudioThread stopped before executing task")));
        }
        s_PendingTasks.store(0);
        // Notify any threads waiting for task completion
        s_CompletionCondition.notify_all();
    }

} // namespace OloEngine::Audio