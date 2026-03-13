#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkThread.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Task/NamedThreads.h"

#include <steam/steamnetworkingsockets.h>

namespace OloEngine
{
    FThread NetworkThread::s_Thread;
    std::atomic<bool> NetworkThread::s_Running{ false };
    u32 NetworkThread::s_TickRateHz = 60;

    void NetworkThread::Start(u32 tickRateHz)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Running.load(std::memory_order_acquire))
        {
            OLO_CORE_WARN("NetworkThread::Start() called when already running");
            return;
        }

        s_TickRateHz = tickRateHz;
        s_Running.store(true, std::memory_order_release);

        s_Thread = FThread(
            "OloEngine::NetworkThread",
            &NetworkThread::ThreadFunc,
            0,
            EThreadPriority::TPri_AboveNormal,
            FThreadAffinity{},
            FThread::NonForkable);

        OLO_CORE_INFO("NetworkThread started (tick rate: {} Hz)", tickRateHz);
    }

    void NetworkThread::Stop()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Running.load(std::memory_order_acquire))
        {
            return;
        }

        s_Running.store(false, std::memory_order_release);

        // Wake the thread so it can notice the stop flag
        Tasks::FNamedThreadManager::Get().WakeThread(Tasks::ENamedThread::NetworkThread);

        if (s_Thread.IsJoinable())
        {
            s_Thread.Join();
        }

        OLO_CORE_INFO("NetworkThread stopped");
    }

    bool NetworkThread::IsRunning()
    {
        return s_Running.load(std::memory_order_acquire);
    }

    void NetworkThread::ThreadFunc()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_TRACE("[NetworkThread] Thread started (ID: {})", FPlatformTLS::GetCurrentThreadId());

        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::NetworkThread);

        while (s_Running.load(std::memory_order_acquire))
        {
            OLO_PROFILE_SCOPE("NetworkThread::Tick");

            // Run GNS callbacks (connection status changes, etc.)
            SteamNetworkingSockets()->RunCallbacks();

            // Process tasks queued to the network thread
            auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::NetworkThread);
            u32 tasksProcessed = Queue.ProcessAll(true);

            // If no tasks were processed, wait briefly for new ones
            if (tasksProcessed == 0 && s_Running.load(std::memory_order_acquire))
            {
                FEventCountToken token = Queue.PrepareWait();
                if (!Queue.HasPendingTasks(true) && s_Running.load(std::memory_order_acquire))
                {
                    u32 sleepMs = (s_TickRateHz > 0) ? (1000 / s_TickRateHz) : 16;
                    Queue.WaitFor(token, FMonotonicTimeSpan::FromMilliseconds(sleepMs));
                }
            }
        }

        // Process remaining tasks before exit
        auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::NetworkThread);
        Queue.ProcessUntilIdle(true);
        Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::NetworkThread);

        OLO_CORE_TRACE("[NetworkThread] Thread exiting");
    }
}
