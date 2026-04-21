// LinuxSemaphore.h - std::counting_semaphore-based implementation
// Mirrors Platform/Windows/WindowsSemaphore.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"

#include <chrono>
#include <semaphore>

namespace OloEngine
{

    class FLinuxSemaphore
    {
      public:
        OLO_NONCOPYABLE(FLinuxSemaphore);

        explicit FLinuxSemaphore(i32 InitialCount)
            : m_Semaphore(InitialCount)
        {
        }

        FLinuxSemaphore(i32 InitialCount, i32 /*MaxCount*/)
            : m_Semaphore(InitialCount)
        {
        }

        void Acquire()
        {
            m_Semaphore.acquire();
        }

        bool TryAcquire()
        {
            return m_Semaphore.try_acquire();
        }

        bool TryAcquireFor(FMonotonicTimeSpan Timeout)
        {
            return m_Semaphore.try_acquire_for(std::chrono::duration<f64>(Timeout.ToSeconds()));
        }

        bool TryAcquireUntil(FMonotonicTimePoint Deadline)
        {
            FMonotonicTimePoint Now = FMonotonicTimePoint::Now();
            if (Deadline <= Now)
            {
                return TryAcquire();
            }
            return TryAcquireFor(Deadline - Now);
        }

        void Release(i32 Count = 1)
        {
            m_Semaphore.release(Count);
        }

      private:
        std::counting_semaphore<> m_Semaphore;
    };

    using FSemaphore = FLinuxSemaphore;

} // namespace OloEngine
