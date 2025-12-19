// ManualResetEvent.h - Platform-specific manual reset event for thread synchronization
// Ported from UE5.7 FPlatformManualResetEvent

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Core/PlatformDetection.h"
#include <atomic>

#if defined(OLO_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

namespace OloEngine
{
    /**
     * Manual reset event that can be waited on and signaled.
     * Uses WaitOnAddress on Windows 8+, futex on Linux, or fallback on other platforms.
     *
     * State values (Linux futex version):
     *   0 = Reset (not notified)
     *   1 = Notified
     */
    class FPlatformManualResetEvent
    {
      public:
        FPlatformManualResetEvent() = default;
        ~FPlatformManualResetEvent() = default;

        // Non-copyable
        FPlatformManualResetEvent(const FPlatformManualResetEvent&) = delete;
        FPlatformManualResetEvent& operator=(const FPlatformManualResetEvent&) = delete;

        // Reset the event to the unsignaled state.
        void Reset()
        {
#if defined(OLO_PLATFORM_LINUX)
            m_State.store(0, std::memory_order_release);
#else
            m_bWait.store(true, std::memory_order_release);
#endif
        }

        // Polls whether the event is in the notified state.
        //
        // @return True if notified, otherwise false.
        bool Poll()
        {
#if defined(OLO_PLATFORM_LINUX)
            return m_State.load(std::memory_order_acquire) != 0;
#else
            return !m_bWait.load(std::memory_order_acquire);
#endif
        }

        // Wait for the event to be signaled.
        void Wait()
        {
#if defined(OLO_PLATFORM_LINUX)
            if (OLO_LIKELY(m_State.load(std::memory_order_acquire) != 0))
            {
                return;
            }
#else
            if (OLO_LIKELY(!m_bWait.load(std::memory_order_acquire)))
            {
                return;
            }
#endif
            WaitSlow();
        }

        // Waits for the wait time for Notify() to be called.
        //
        // Notify() may be called prior to WaitFor(), and this will return immediately in that case.
        //
        // @param WaitTime   Relative time after which waiting is canceled and the thread wakes.
        // @return True if Notify() was called before the wait time elapsed, otherwise false.
        bool WaitFor(FMonotonicTimeSpan WaitTime)
        {
#if defined(OLO_PLATFORM_LINUX)
            return m_State.load(std::memory_order_acquire) != 0 || WaitForSlow(WaitTime);
#else
            return !m_bWait.load(std::memory_order_acquire) || WaitForSlow(WaitTime);
#endif
        }

        // Waits until the wait time for Notify() to be called.
        //
        // Notify() may be called prior to WaitUntil(), and this will return immediately in that case.
        //
        // @param WaitTime   Absolute time after which waiting is canceled and the thread wakes.
        // @return True if Notify() was called before the wait time elapsed, otherwise false.
        bool WaitUntil(FMonotonicTimePoint WaitTime)
        {
#if defined(OLO_PLATFORM_LINUX)
            return m_State.load(std::memory_order_acquire) != 0 || WaitUntilSlow(WaitTime);
#else
            return !m_bWait.load(std::memory_order_acquire) || WaitUntilSlow(WaitTime);
#endif
        }

        // Signal the event, waking waiting threads.
        void Notify();

      private:
        void WaitSlow();
        bool WaitForSlow(FMonotonicTimeSpan WaitTime);
        bool WaitUntilSlow(FMonotonicTimePoint WaitTime);

#if defined(OLO_PLATFORM_LINUX)
        // Linux futex uses uint32 state: 0 = Reset, 1 = Notified
        std::atomic<u32> m_State{ 0 };
#else
        // Windows and fallback use bool: true = waiting, false = notified
        std::atomic<bool> m_bWait{ true };
#endif
    };

    // Alias for general use - most code should use this
    using FManualResetEvent = FPlatformManualResetEvent;

} // namespace OloEngine
