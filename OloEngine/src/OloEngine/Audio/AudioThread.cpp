#include "OloEnginePCH.h"
#include "AudioThread.h"

#include <chrono>

// Platform-specific includes for thread priority
#if defined(OLO_PLATFORM_LINUX) || defined(OLO_PLATFORM_MACOS)
    #include <pthread.h>
    #include <cerrno>
#endif

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

        // Check if we're trying to stop from within the audio thread itself
        if (std::this_thread::get_id() == s_AudioThreadID.load(std::memory_order_acquire))
        {
            // CRITICAL: Self-stop creates unsafe state transition. After detaching the thread,
            // it continues executing but s_IsRunning is set to false, creating a race where
            // external checks report the thread as stopped while it's still processing tasks.
            // Another thread could call Start() and potentially create a second audio thread.
            // Solution: Disallow self-stop entirely.
            OLO_CORE_ERROR("AudioThread::Stop() called from within audio thread - self-stop is not allowed. "
                          "The audio thread cannot stop itself safely. Call Stop() from a different thread.");
            return;
        }

        s_ShouldStop.store(true);
        s_TaskCondition.notify_all();

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
        #if defined(OLO_PLATFORM_WINDOWS)
            // Windows: Set to time-critical priority
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
            {
                OLO_CORE_WARN("AudioThread: Failed to set Windows thread priority to TIME_CRITICAL!");
            }
            else
            {
                OLO_CORE_TRACE("AudioThread: Set thread priority to TIME_CRITICAL.");
            }
        #elif defined(OLO_PLATFORM_LINUX) || defined(OLO_PLATFORM_MACOS)
            // POSIX (Linux/macOS): Set real-time scheduling policy
            pthread_t thread = pthread_self();
            
            // Get the minimum and maximum priority for SCHED_FIFO
            i32 minPriority = sched_get_priority_min(SCHED_FIFO);
            i32 maxPriority = sched_get_priority_max(SCHED_FIFO);
            
            if (minPriority == -1 || maxPriority == -1)
            {
                OLO_CORE_WARN("AudioThread: Failed to get SCHED_FIFO priority range (errno: {})", errno);
            }
            else
            {
                // Set to high priority (75% of the range above minimum)
                sched_param schedParam;
                schedParam.sched_priority = minPriority + ((maxPriority - minPriority) * 3 / 4);
                
                i32 result = pthread_setschedparam(thread, SCHED_FIFO, &schedParam);
                if (result != 0)
                {
                    // Note: This often requires elevated privileges (CAP_SYS_NICE on Linux or running as root)
                    // Fall back to trying SCHED_RR
                    schedParam.sched_priority = sched_get_priority_min(SCHED_RR) + 
                                                ((sched_get_priority_max(SCHED_RR) - sched_get_priority_min(SCHED_RR)) * 3 / 4);
                    result = pthread_setschedparam(thread, SCHED_RR, &schedParam);
                    
                    if (result != 0)
                    {
                        OLO_CORE_WARN("AudioThread: Failed to set real-time scheduling (SCHED_FIFO/SCHED_RR). "
                                      "Error code: {}. Audio thread will use default scheduling. "
                                      "Note: Real-time priority typically requires elevated privileges.", result);
                    }
                    else
                    {
                        OLO_CORE_TRACE("AudioThread: Set thread scheduling to SCHED_RR with priority {}", schedParam.sched_priority);
                    }
                }
                else
                {
                    OLO_CORE_TRACE("AudioThread: Set thread scheduling to SCHED_FIFO with priority {}", schedParam.sched_priority);
                }
            }
        #else
            // Unsupported platform - no priority adjustment
            OLO_CORE_INFO("AudioThread: Real-time thread priority setting not implemented for this platform. "
                          "Audio thread will use default scheduling priority.");
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
        // Important: Only clear s_IsRunning if we're still the registered audio thread.
        // If Stop() was called from within this thread (self-stop), another thread may have
        // already started a new audio thread. We must not overwrite the new thread's state.
        std::thread::id currentThreadID = std::this_thread::get_id();
        std::thread::id registeredThreadID = s_AudioThreadID.load(std::memory_order_acquire);
        
        if (currentThreadID == registeredThreadID)
        {
            // We're still the registered audio thread - safe to clear state
            s_IsRunning.store(false, std::memory_order_release);
        }
        else
        {
            // Another thread has already started - don't touch s_IsRunning
            OLO_CORE_TRACE("AudioThread: Exiting old thread (ID: {}) - new thread already started (ID: {})",
                           std::hash<std::thread::id>{}(currentThreadID),
                           std::hash<std::thread::id>{}(registeredThreadID));
        }
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
