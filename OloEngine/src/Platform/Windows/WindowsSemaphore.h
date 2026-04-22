// WindowsSemaphore.h - Win32 counting semaphore implementation
// Ported from UE5.7 Windows/WindowsSemaphore.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/MonotonicTime.h"

#include <algorithm>
#include <climits>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace OloEngine
{

    /**
     * Windows implementation of a counting semaphore.
     *
     * A semaphore is a synchronization primitive that maintains a count.
     * - Acquire() decrements the count and blocks if the count would go negative
     * - Release() increments the count and potentially wakes waiting threads
     */
    class FWindowsSemaphore
    {
      public:
        OLO_NONCOPYABLE(FWindowsSemaphore);

        /**
         * Construct a semaphore with initial count.
         * @param InitialCount The initial count of the semaphore (must be >= 0)
         */
        explicit FWindowsSemaphore(i32 InitialCount)
        {
            OLO_CORE_CHECK_SLOW(InitialCount >= 0);
            m_Semaphore = CreateSemaphoreW(nullptr, InitialCount, LONG_MAX, nullptr);
            if (m_Semaphore == nullptr)
            {
                OLO_CORE_ERROR("FWindowsSemaphore: CreateSemaphoreW failed (GetLastError={})",
                               static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "CreateSemaphoreW failed");
            }
        }

        /**
         * Construct a semaphore with initial and maximum count.
         * @param InitialCount The initial count of the semaphore (must be >= 0 and <= MaxCount)
         * @param MaxCount The maximum count the semaphore can reach
         */
        FWindowsSemaphore(i32 InitialCount, i32 MaxCount)
        {
            OLO_CORE_CHECK_SLOW(InitialCount >= 0);
            OLO_CORE_CHECK_SLOW(MaxCount > 0);
            OLO_CORE_CHECK_SLOW(InitialCount <= MaxCount);
            m_Semaphore = CreateSemaphoreW(nullptr, InitialCount, MaxCount, nullptr);
            if (m_Semaphore == nullptr)
            {
                OLO_CORE_ERROR("FWindowsSemaphore: CreateSemaphoreW failed (GetLastError={})",
                               static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "CreateSemaphoreW failed");
            }
        }

        ~FWindowsSemaphore()
        {
            if (m_Semaphore != nullptr)
            {
                if (!CloseHandle(m_Semaphore))
                {
                    // Non-fatal but should never happen for a handle we own; log
                    // GetLastError so silent handle leaks don't slip through.
                    OLO_CORE_ERROR("FWindowsSemaphore: CloseHandle failed (GetLastError={})",
                                   static_cast<u32>(GetLastError()));
                    OLO_CORE_ASSERT(false, "CloseHandle failed in ~FWindowsSemaphore");
                }
                m_Semaphore = nullptr;
            }
        }

        /**
         * Acquires the semaphore, blocking until the count is positive.
         * Decrements the count by 1.
         */
        void Acquire()
        {
            DWORD Res = WaitForSingleObject(m_Semaphore, INFINITE);
            if (Res != WAIT_OBJECT_0)
            {
                // WAIT_FAILED / WAIT_ABANDONED are non-recoverable for a semaphore wait:
                // log and assert so bugs surface deterministically in every configuration,
                // instead of silently returning with the count in an unknown state.
                OLO_CORE_ERROR("FWindowsSemaphore::Acquire: WaitForSingleObject returned {} (GetLastError={})",
                               static_cast<u32>(Res), static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "WaitForSingleObject failed in FWindowsSemaphore::Acquire");
            }
        }

        /**
         * Tries to acquire the semaphore without blocking.
         * @return true if the semaphore was acquired, false otherwise
         */
        bool TryAcquire()
        {
            DWORD Res = WaitForSingleObject(m_Semaphore, 0);
            if (Res != WAIT_OBJECT_0 && Res != WAIT_TIMEOUT)
            {
                OLO_CORE_ERROR("FWindowsSemaphore::TryAcquire: WaitForSingleObject returned {} (GetLastError={})",
                               static_cast<u32>(Res), static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "WaitForSingleObject failed in FWindowsSemaphore::TryAcquire");
                return false;
            }
            return Res == WAIT_OBJECT_0;
        }

        /**
         * Tries to acquire the semaphore within the given timeout.
         * @param Timeout The maximum duration to wait
         * @return true if the semaphore was acquired, false if timed out
         */
        bool TryAcquireFor(FMonotonicTimeSpan Timeout)
        {
            f64 ms = Timeout.ToMilliseconds();
            if (std::isnan(ms))
            {
                ms = 0.0;
            }
            ms = (std::max)(0.0, ms);
            // INFINITE is the WinAPI sentinel for "wait forever"; clamp any value at or
            // above it (and any value that would overflow DWORD) to INFINITE to avoid
            // wrapping around to an unintended short timeout.
            const DWORD TimeoutMs = (ms >= static_cast<f64>(INFINITE))
                                        ? INFINITE
                                        : static_cast<DWORD>(ms);
            DWORD Res = WaitForSingleObject(m_Semaphore, TimeoutMs);
            if (Res != WAIT_OBJECT_0 && Res != WAIT_TIMEOUT)
            {
                OLO_CORE_ERROR("FWindowsSemaphore::TryAcquireFor: WaitForSingleObject returned {} (GetLastError={})",
                               static_cast<u32>(Res), static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "WaitForSingleObject failed in FWindowsSemaphore::TryAcquireFor");
                return false;
            }
            return Res == WAIT_OBJECT_0;
        }

        /**
         * Tries to acquire the semaphore until the given deadline.
         * @param Deadline The absolute time point to wait until
         * @return true if the semaphore was acquired, false if timed out
         */
        bool TryAcquireUntil(FMonotonicTimePoint Deadline)
        {
            auto Now = FMonotonicTimePoint::Now();
            if (Deadline <= Now)
            {
                return TryAcquire();
            }
            return TryAcquireFor(Deadline - Now);
        }

        /**
         * Releases the semaphore, incrementing the count.
         * @param Count The number to add to the semaphore count (must be > 0)
         */
        void Release(i32 Count = 1)
        {
            OLO_CORE_CHECK_SLOW(Count > 0);
            BOOL bRes = ReleaseSemaphore(m_Semaphore, Count, nullptr);
            if (!bRes)
            {
                // Most commonly: count would exceed max (caller bug) or handle is invalid.
                // Surface this in every build rather than silently losing Release() calls
                // which would corrupt the waiter count.
                OLO_CORE_ERROR("FWindowsSemaphore::Release: ReleaseSemaphore failed Count={} (GetLastError={})",
                               Count, static_cast<u32>(GetLastError()));
                OLO_CORE_ASSERT(false, "ReleaseSemaphore failed");
            }
        }

      private:
        HANDLE m_Semaphore = nullptr;
    };

    using FSemaphore = FWindowsSemaphore;

} // namespace OloEngine
