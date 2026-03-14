#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/Thread.h"

#include <atomic>

namespace OloEngine
{
    class NetworkThread
    {
      public:
        static void Start(u32 tickRateHz = 60);
        static void Stop();
        static bool IsRunning();
        [[nodiscard]] static u32 GetTickRate();

      private:
        static void ThreadFunc();

        static FThread s_Thread;
        static std::atomic<bool> s_Running;
        static std::atomic<u32> s_TickRateHz;
    };
} // namespace OloEngine
