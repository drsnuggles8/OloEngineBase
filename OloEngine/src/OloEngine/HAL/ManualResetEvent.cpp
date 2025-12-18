// ManualResetEvent.cpp - Platform-specific manual reset event implementation
// Ported from UE5.7 FMicrosoftManualResetEvent / FUnixManualResetEvent

#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Core/PlatformTime.h"
#include <cmath>

#if defined(OLO_PLATFORM_LINUX)
    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>
    #include <climits>
    #include <time.h>
#endif

namespace OloEngine
{

#if defined(OLO_PLATFORM_WINDOWS)
	// Windows implementation using WaitOnAddress (Windows 8+)

	void FPlatformManualResetEvent::WaitSlow()
	{
		LowLevelTasks::FOversubscriptionScope Scope;
		for (;;)
		{
			bool bLocalWait = m_bWait.load(std::memory_order_acquire);
			if (!bLocalWait)
			{
				return;
			}
			// WaitOnAddress blocks until the value at the address changes
			::WaitOnAddress(&m_bWait, &bLocalWait, sizeof(bool), INFINITE);
		}
	}

	bool FPlatformManualResetEvent::WaitForSlow(FMonotonicTimeSpan WaitTime)
	{
		bool bLocalWait = m_bWait.load(std::memory_order_acquire);
		if (!bLocalWait || WaitTime <= FMonotonicTimeSpan::Zero())
		{
			return !bLocalWait;
		}

		LowLevelTasks::FOversubscriptionScope Scope;

		// Clamp to INFINITE. Test against INFINITE - 1 because of the ceiling operation.
		const f64 RawWaitMs = WaitTime.ToMilliseconds();
		const DWORD WaitMs = RawWaitMs > static_cast<f64>(INFINITE - 1) ? INFINITE : static_cast<DWORD>(CeilToInt64(RawWaitMs));

		const bool bTimedOut = !::WaitOnAddress(&m_bWait, &bLocalWait, sizeof(bool), WaitMs) && GetLastError() == ERROR_TIMEOUT;
		bLocalWait = m_bWait.load(std::memory_order_acquire);
		if (OLO_LIKELY(!bLocalWait || bTimedOut))
		{
			return !bLocalWait;
		}

		// Handle a spurious wake by waiting until the wait time has elapsed one more time because WaitUntilSlow
		// handles spurious wakes in a loop and avoids exceeding the originally requested wake time by more than
		// the typical variation due to scheduling imprecision.
		return WaitUntilSlow(FMonotonicTimePoint::Now() + WaitTime);
	}

	bool FPlatformManualResetEvent::WaitUntilSlow(FMonotonicTimePoint WaitTime)
	{
		FMonotonicTimeSpan WaitSpan = WaitTime - FMonotonicTimePoint::Now();
		LowLevelTasks::FOversubscriptionScope Scope(WaitSpan > FMonotonicTimeSpan::Zero());

		for (;;)
		{
			bool bLocalWait = m_bWait.load(std::memory_order_acquire);
			if (!bLocalWait || WaitSpan <= FMonotonicTimeSpan::Zero())
			{
				return !bLocalWait;
			}
			const DWORD WaitMs = WaitTime.IsInfinity() ? INFINITE : static_cast<DWORD>(CeilToInt64(WaitSpan.ToMilliseconds()));
			::WaitOnAddress(&m_bWait, &bLocalWait, sizeof(bool), WaitMs);
			WaitSpan = WaitTime - FMonotonicTimePoint::Now();
		}
	}

	void FPlatformManualResetEvent::Notify()
	{
		m_bWait.store(false, std::memory_order_release);
		// Wake ALL waiters - this is a manual reset event
		::WakeByAddressAll(reinterpret_cast<void*>(&m_bWait));
	}

#elif defined(OLO_PLATFORM_LINUX)
	// Linux implementation using futex syscall

	namespace
	{
		/**
		 * Convert seconds to timespec for futex syscall.
		 * @param Seconds Time in seconds
		 * @return timespec structure with seconds and nanoseconds
		 */
		timespec SecondsToTimeSpec(f64 Seconds)
		{
			timespec TimeSpec;
			TimeSpec.tv_sec = static_cast<time_t>(Seconds);
			TimeSpec.tv_nsec = static_cast<long>((Seconds - static_cast<f64>(TimeSpec.tv_sec)) * 1000000000.0);
			return TimeSpec;
		}
	}

	void FPlatformManualResetEvent::WaitSlow()
	{
		LowLevelTasks::FOversubscriptionScope Scope;
		for (;;)
		{
			// Wait until State becomes non-zero (notified)
			if (m_State.load(std::memory_order_acquire) != 0)
			{
				return;
			}
			// FUTEX_WAIT_PRIVATE: wait if *&m_State == 0
			syscall(SYS_futex, &m_State, FUTEX_WAIT_PRIVATE, 0, nullptr, nullptr, 0);
		}
	}

	bool FPlatformManualResetEvent::WaitForSlow(FMonotonicTimeSpan WaitTime)
	{
		u32 LocalState = m_State.load(std::memory_order_acquire);
		if (LocalState != 0 || WaitTime <= FMonotonicTimeSpan::Zero())
		{
			return LocalState != 0;
		}

		LowLevelTasks::FOversubscriptionScope Scope;

		const f64 WaitSeconds = WaitTime.ToSeconds();
		timespec TimeSpec = SecondsToTimeSpec(WaitSeconds);

		// FUTEX_WAIT_PRIVATE with timeout (relative time)
		syscall(SYS_futex, &m_State, FUTEX_WAIT_PRIVATE, 0, &TimeSpec, nullptr, 0);

		LocalState = m_State.load(std::memory_order_acquire);
		if (LocalState != 0)
		{
			return true;
		}

		// Handle a spurious wake by waiting until the wait time has elapsed
		return WaitUntilSlow(FMonotonicTimePoint::Now() + WaitTime);
	}

	bool FPlatformManualResetEvent::WaitUntilSlow(FMonotonicTimePoint WaitTime)
	{
		LowLevelTasks::FOversubscriptionScope Scope;

		for (;;)
		{
			if (m_State.load(std::memory_order_acquire) != 0)
			{
				return true;
			}

			FMonotonicTimeSpan WaitSpan = WaitTime - FMonotonicTimePoint::Now();
			if (WaitSpan <= FMonotonicTimeSpan::Zero())
			{
				return m_State.load(std::memory_order_acquire) != 0;
			}

			if (WaitTime.IsInfinity())
			{
				// Infinite wait
				syscall(SYS_futex, &m_State, FUTEX_WAIT_PRIVATE, 0, nullptr, nullptr, 0);
			}
			else
			{
				// Get absolute time for FUTEX_WAIT_BITSET_PRIVATE with CLOCK_REALTIME
				// Note: FUTEX_WAIT_BITSET_PRIVATE with FUTEX_CLOCK_REALTIME uses absolute time
				const f64 AbsoluteSeconds = FPlatformTime::Seconds() + WaitSpan.ToSeconds();
				timespec AbsTimeSpec = SecondsToTimeSpec(AbsoluteSeconds);

				// Use FUTEX_WAIT_BITSET_PRIVATE which supports absolute timeouts
				// FUTEX_BITSET_MATCH_ANY (0xFFFFFFFF) matches any bit
				constexpr u32 FUTEX_BITSET_MATCH_ANY = 0xFFFFFFFF;
				syscall(SYS_futex, &m_State, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 
				        0, &AbsTimeSpec, nullptr, FUTEX_BITSET_MATCH_ANY);
			}
		}
	}

	void FPlatformManualResetEvent::Notify()
	{
		if (m_State.exchange(1, std::memory_order_release) == 0)
		{
			// Wake all waiting threads (INT_MAX)
			syscall(SYS_futex, &m_State, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
		}
	}

#else
	// Generic fallback using spin-wait with timeout support
	// Note: This is less efficient than futex on Linux or WaitOnAddress on Windows

	void FPlatformManualResetEvent::WaitSlow()
	{
		LowLevelTasks::FOversubscriptionScope Scope;
		while (m_bWait.load(std::memory_order_acquire))
		{
			FPlatformProcess::Yield();
		}
	}

	bool FPlatformManualResetEvent::WaitForSlow(FMonotonicTimeSpan WaitTime)
	{
		return WaitUntilSlow(FMonotonicTimePoint::Now() + WaitTime);
	}

	bool FPlatformManualResetEvent::WaitUntilSlow(FMonotonicTimePoint WaitTime)
	{
		LowLevelTasks::FOversubscriptionScope Scope;
		while (m_bWait.load(std::memory_order_acquire))
		{
			if (FMonotonicTimePoint::Now() >= WaitTime)
			{
				return !m_bWait.load(std::memory_order_acquire);
			}
			FPlatformProcess::Yield();
		}
		return true;
	}

	void FPlatformManualResetEvent::Notify()
	{
		m_bWait.store(false, std::memory_order_release);
	}

#endif

} // namespace OloEngine
