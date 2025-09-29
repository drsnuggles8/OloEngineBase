#pragma once

#include "OloEngine/Core/Base.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <future>

namespace OloEngine::Audio
{
    /// High-priority audio thread manager for real-time audio processing
    /// Provides lock-free communication between main thread and audio thread
    class AudioThread
    {
    public:
        using Task = std::function<void()>;
        
    private:
        /// Completion token for per-task tracking
        struct CompletionToken
        {
            Task task;
            std::promise<void> promise;
            
            CompletionToken(Task t) : task(std::move(t)) {}
        };
        
    public:

        /// Start the audio thread
        static bool Start();

        /// Stop the audio thread
        static void Stop();

        /// Check if the audio thread is currently running
        static bool IsRunning();

        /// Check if the current thread is the audio thread
        static bool IsAudioThread();

        /// Get the audio thread ID
        static std::thread::id GetThreadID();

        /// Execute a task on the audio thread
        /// @param task - Function to execute on audio thread
        /// @param waitForCompletion - Whether to wait for task completion
        /// @return Future that completes when the specific task finishes
        static std::future<void> ExecuteOnAudioThread(Task task, bool waitForCompletion = true);

        /// Get the number of pending tasks in the audio thread queue
        static size_t GetPendingTaskCount();

    private:
        AudioThread() = delete;
        ~AudioThread() = delete;

        static void AudioThreadLoop();
        static void ProcessTasks();

        // Thread management
        static std::unique_ptr<std::thread> s_AudioThread;
        static std::atomic<bool> s_ShouldStop;
        static std::atomic<bool> s_IsRunning;
        static std::thread::id s_AudioThreadID;

        // Task queue (lock-free would be better, but using mutex for simplicity)
        static std::queue<std::unique_ptr<CompletionToken>> s_TaskQueue;
        static std::mutex s_TaskQueueMutex;
        static std::mutex s_StartStopMutex;  // Serializes start/stop operations
        static std::condition_variable s_TaskCondition;
        static std::condition_variable s_CompletionCondition;
        static std::atomic<int> s_PendingTasks;
    };

} // namespace OloEngine::Audio