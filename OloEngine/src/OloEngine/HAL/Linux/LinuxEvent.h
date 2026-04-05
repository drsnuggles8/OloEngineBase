// Linux implementation of FEvent using pthreads
// Mirrors OloEngine/HAL/Windows/WindowsEvent.h

#pragma once

#include "OloEngine/HAL/Event.h"

#include <pthread.h>
#include <ctime>
#include <cerrno>

namespace OloEngine
{
    class FEventLinux : public FEvent
    {
      public:
        FEventLinux()
            : m_Triggered(false), m_ManualReset(false)
        {
            pthread_mutex_init(&m_Mutex, nullptr);
            pthread_cond_init(&m_Condition, nullptr);
        }

        ~FEventLinux() override
        {
            pthread_cond_destroy(&m_Condition);
            pthread_mutex_destroy(&m_Mutex);
        }

        // Non-copyable, non-movable
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
            pthread_mutex_lock(&m_Mutex);
            m_Triggered = true;
            if (m_ManualReset)
            {
                pthread_cond_broadcast(&m_Condition);
            }
            else
            {
                pthread_cond_signal(&m_Condition);
            }
            pthread_mutex_unlock(&m_Mutex);
        }

        void Reset() override
        {
            ResetForStats();
            pthread_mutex_lock(&m_Mutex);
            m_Triggered = false;
            pthread_mutex_unlock(&m_Mutex);
        }

        bool Wait(u32 WaitTime, [[maybe_unused]] bool bIgnoreThreadIdleStats = false) override
        {
            WaitForStats();

            pthread_mutex_lock(&m_Mutex);

            if (WaitTime == 0xFFFFFFFF) // INFINITE
            {
                while (!m_Triggered)
                {
                    pthread_cond_wait(&m_Condition, &m_Mutex);
                }
            }
            else
            {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += WaitTime / 1000;
                ts.tv_nsec += (WaitTime % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L)
                {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }

                while (!m_Triggered)
                {
                    if (int rc = pthread_cond_timedwait(&m_Condition, &m_Mutex, &ts); rc == ETIMEDOUT)
                    {
                        break;
                    }
                }
            }

            bool wasTriggered = m_Triggered;

            // Auto-reset events reset after a successful wait
            if (!m_ManualReset && wasTriggered)
            {
                m_Triggered = false;
            }

            pthread_mutex_unlock(&m_Mutex);
            return wasTriggered;
        }

      private:
        pthread_mutex_t m_Mutex;
        pthread_cond_t m_Condition;
        bool m_Triggered;
        bool m_ManualReset;
    };

} // namespace OloEngine
