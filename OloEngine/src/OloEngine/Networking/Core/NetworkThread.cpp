#include "OloEnginePCH.h"
#include "NetworkThread.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <chrono>
#include <thread>

namespace OloEngine
{
    FThread           NetworkThread::s_Thread;
    std::atomic<bool> NetworkThread::s_Running{ false };
    u32               NetworkThread::s_TickRateHz = 60;

    void NetworkThread::Start(u32 tickRateHz)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Running.load(std::memory_order_acquire))
        {
            OLO_CORE_WARN("[NetworkThread] Already running.");
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

        OLO_CORE_TRACE("[NetworkThread] Started at {} Hz.", tickRateHz);
    }

    void NetworkThread::Stop()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Running.load(std::memory_order_acquire))
        {
            return;
        }

        OLO_CORE_TRACE("[NetworkThread] Stopping...");

        s_Running.store(false, std::memory_order_release);

        // Wake the thread if it is waiting on the named-thread queue
        Tasks::FNamedThreadManager::Get().WakeThread(Tasks::ENamedThread::NetworkThread);

        if (s_Thread.IsJoinable())
        {
            s_Thread.Join();
        }

        OLO_CORE_TRACE("[NetworkThread] Stopped.");
    }

    bool NetworkThread::IsRunning()
    {
        return s_Running.load(std::memory_order_acquire);
    }

    void NetworkThread::ThreadFunc()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_TRACE("[NetworkThread] Thread started.");

        // Attach to the named-thread system so cross-thread tasks can be dispatched here
        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::NetworkThread);

        const u64 tickIntervalUs = (s_TickRateHz > 0)
            ? static_cast<u64>(1'000'000u / s_TickRateHz)
            : 16'666u; // ~60 Hz default

        while (s_Running.load(std::memory_order_acquire))
        {
            OLO_PROFILE_SCOPE("NetworkThread::Tick");

            auto tickStart = std::chrono::steady_clock::now();

            // --- Poll GNS for connection-state callbacks ---
            ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
            if (pInterface)
            {
                pInterface->RunCallbacks();
            }

            // --- Drain tasks queued to the network named thread ---
            auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::NetworkThread);
            Queue.ProcessAll(true); // true = include local queue

            // --- Sleep for the remainder of the tick interval ---
            auto tickEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart).count();

            if (elapsed < static_cast<i64>(tickIntervalUs))
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<i64>(tickIntervalUs) - elapsed));
            }
        }

        // Drain remaining tasks before exit
        auto& Queue = Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::NetworkThread);
        Queue.ProcessUntilIdle(true);

        Tasks::FNamedThreadManager::Get().DetachFromThread(Tasks::ENamedThread::NetworkThread);

        OLO_CORE_TRACE("[NetworkThread] Thread exiting.");
    }

} // namespace OloEngine
