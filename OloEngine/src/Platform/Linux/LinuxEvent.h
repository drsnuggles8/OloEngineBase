// Linux implementation of FEvent using pthreads
// Mirrors Platform/Windows/WindowsEvent.h

#pragma once

#include "OloEngine/HAL/Event.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace OloEngine
{
    class FEventLinux : public FEvent
    {
      public:
        FEventLinux() = default;
        ~FEventLinux() override = default;

        FEventLinux(const FEventLinux&) = delete;
        FEventLinux& operator=(const FEventLinux&) = delete;
        FEventLinux(FEventLinux&&) = delete;
        FEventLinux& operator=(FEventLinux&&) = delete;

        [[deprecated("Direct creation of FEvent is discouraged. Use FEventRef instead.")]]
        bool Create(bool bIsManualReset = false) override
        {
            m_ManualReset = bIsManualReset;
            m_Triggered = false;
            return true;
        }

        bool IsManualReset() override
        {
            return m_ManualReset;
        }

        void Trigger() override
        {
            TriggerForStats();
            {
                std::lock_guard lock(m_Mutex);
                m_Triggered = true;
                ++m_Generation;
            }
            if (m_ManualReset)
            {
                m_Condition.notify_all();
            }
            else
            {
                m_Condition.notify_one();
            }
        }

        void Reset() override
        {
            ResetForStats();
            std::lock_guard lock(m_Mutex);
            m_Triggered = false;
        }

        bool Wait(u32 WaitTime, [[maybe_unused]] bool bIgnoreThreadIdleStats = false) override
        {
            WaitForStats();

            std::unique_lock lock(m_Mutex);
            const u64 initialGeneration = m_Generation;

            // Manual reset: a Trigger releases every thread waiting at that
            // moment, even when a Reset follows before the thread runs again, so
            // a trigger since this wait began counts (the generation).
            // Auto reset: a Trigger releases one thread, the one that takes
            // m_Triggered. A thread that wakes to find it taken keeps waiting.
            auto predicate = [this, initialGeneration]
            { return m_Triggered || (m_ManualReset && m_Generation != initialGeneration); };

            if (WaitTime == 0xFFFFFFFF) // INFINITE
            {
                m_Condition.wait(lock, predicate);
            }
            else
            {
                if (!m_Condition.wait_for(lock, std::chrono::milliseconds(WaitTime), predicate))
                {
                    return false; // Timed out without predicate becoming true
                }
            }

            // The predicate held, so this wait was released: returning
            // m_Triggered here reported a released wait as failed whenever
            // another thread had cleared the flag first.
            // Auto-reset events reset after a successful wait
            if (!m_ManualReset)
            {
                m_Triggered = false;
            }

            return true;
        }

      private:
        std::mutex m_Mutex;
        std::condition_variable m_Condition;
        u64 m_Generation{ 0 };
        bool m_Triggered{ false };
        bool m_ManualReset{ false };
    };

} // namespace OloEngine
