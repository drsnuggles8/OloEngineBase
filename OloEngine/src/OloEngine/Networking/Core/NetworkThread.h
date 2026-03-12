#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/Thread.h"

#include <atomic>

namespace OloEngine
{
    // @class NetworkThread
    // @brief Dedicated thread for polling GameNetworkingSockets callbacks and
    //        draining incoming message queues.
    //
    // Follows the AudioEngine pattern:
    //   - Start() registers the thread with FNamedThreadManager as ENamedThread::NetworkThread
    //   - Stop() signals shutdown and joins the thread
    //   - Cross-thread communication uses EnqueueNetworkThreadTask /
    //     EnqueueGameThreadTask (same mechanism as audio, physics cooking, etc.)
    class NetworkThread
    {
      public:
        static void Start(u32 tickRateHz = 60);
        static void Stop();

        [[nodiscard]] static bool IsRunning();

      private:
        static void ThreadFunc();

        static FThread           s_Thread;
        static std::atomic<bool> s_Running;
        static u32               s_TickRateHz;
    };

} // namespace OloEngine
