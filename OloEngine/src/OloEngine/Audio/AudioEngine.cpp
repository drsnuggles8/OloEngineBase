#include "OloEnginePCH.h"
#include "AudioEngine.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Debug/Profiler.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace OloEngine
{
    ma_engine* AudioEngine::s_Engine = nullptr;
    FThread AudioEngine::s_AudioThread;
    std::atomic<bool> AudioEngine::s_AudioThreadRunning{ false };

    bool AudioEngine::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Engine)
        {
            OLO_CORE_WARN("[AudioEngine] Already initialized.");
            return true;
        }

        OLO_CORE_TRACE("[AudioEngine] Initializing.");
        ma_engine_config config = ::ma_engine_config_init();
        config.listenerCount = 1;

        s_Engine = new ma_engine();
        ma_result result = ::ma_engine_init(&config, s_Engine);

        if (result == MA_SUCCESS)
        {
            OLO_CORE_TRACE("[AudioEngine] Initialized successfully with sample rate {}", ma_engine_get_sample_rate(s_Engine));

            // Start dedicated audio thread with TimeCritical priority
            s_AudioThreadRunning.store(true, std::memory_order_release);
            s_AudioThread = FThread(
                "OloEngine::AudioThread",
                &AudioEngine::AudioThreadFunc,
                0, // Default stack size
                EThreadPriority::TPri_TimeCritical,
                FThreadAffinity{},
                FThread::NonForkable);

            OLO_CORE_TRACE("[AudioEngine] Audio thread started with TimeCritical priority");
            return true;
        }
        else
        {
            OLO_CORE_ERROR("[AudioEngine] Failed to initialize audio engine! Error code: {}", static_cast<i32>(result));
            delete s_Engine;
            s_Engine = nullptr;
            OLO_CORE_ASSERT(false, "Failed to initialize audio engine!");
            return false;
        }
    }

    void AudioEngine::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_TRACE("[AudioEngine] Shutting down.");

        // Stop audio thread first
        if (s_AudioThreadRunning.load(std::memory_order_acquire))
        {
            OLO_CORE_TRACE("[AudioEngine] Stopping audio thread...");
            s_AudioThreadRunning.store(false, std::memory_order_release);

            // Wake up the audio thread if it's waiting
            Tasks::FNamedThreadManager::Get().WakeThread(Tasks::ENamedThread::AudioThread);

            // Wait for thread to finish
            if (s_AudioThread.IsJoinable())
            {
                s_AudioThread.Join();
            }
            OLO_CORE_TRACE("[AudioEngine] Audio thread stopped.");
        }

        if (s_Engine)
        {
            ::ma_engine_uninit(s_Engine);
            delete s_Engine;
            s_Engine = nullptr;
        }

        OLO_CORE_TRACE("[AudioEngine] Shutdown complete.");
    }

    [[nodiscard("Store this!")]] AudioEngineInternal AudioEngine::GetEngine()
    {
        return s_Engine;
    }

    void AudioEngine::AudioThreadFunc()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_TRACE("[AudioEngine] Audio thread started (ID: {})", FPlatformTLS::GetCurrentThreadId());

        // Attach this thread to the AudioThread named thread system
        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::AudioThread);

        // Main audio thread loop - process tasks from the named thread queue
        while (s_AudioThreadRunning.load(std::memory_order_acquire))
        {
            OLO_PROFILE_SCOPE("AudioThread::ProcessTasks");

            // Get the audio thread's task queue
            auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::AudioThread);

            // Process all pending audio tasks
            u32 TasksProcessed = Queue.ProcessAll(true); // true = include local queue

            // If no tasks were processed, wait for notification to avoid busy-waiting
            if (TasksProcessed == 0 && s_AudioThreadRunning.load(std::memory_order_acquire))
            {
                // Prepare to wait for tasks
                FEventCountToken Token = Queue.PrepareWait();

                // Double-check that there are no tasks and we should still wait
                if (!Queue.HasPendingTasks(true) && s_AudioThreadRunning.load(std::memory_order_acquire))
                {
                    // Wait with timeout to periodically check s_AudioThreadRunning
                    // 10ms timeout gives responsive shutdown while avoiding busy-waiting
                    Queue.WaitFor(Token, FMonotonicTimeSpan::FromMilliseconds(10));
                }
            }
        }

        // Process remaining tasks before exit
        OLO_CORE_TRACE("[AudioEngine] Audio thread shutting down, processing remaining tasks...");
        auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::AudioThread);
        Queue.ProcessUntilIdle(true);

        // Detach from named thread system
        Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::AudioThread);

        OLO_CORE_TRACE("[AudioEngine] Audio thread exiting");
    }
} // namespace OloEngine
