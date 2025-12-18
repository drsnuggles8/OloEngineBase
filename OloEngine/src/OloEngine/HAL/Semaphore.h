// Semaphore.h - Counting semaphore for thread synchronization
// Ported from UE5.7 Windows/WindowsSemaphore.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/MonotonicTime.h"

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace OloEngine
{

#ifdef OLO_PLATFORM_WINDOWS

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
		: m_Semaphore(CreateSemaphoreW(nullptr, InitialCount, LONG_MAX, nullptr))
	{
		OLO_CORE_CHECK_SLOW(m_Semaphore != nullptr);
	}

	/**
	 * Construct a semaphore with initial and maximum count.
	 * @param InitialCount The initial count of the semaphore (must be >= 0 and <= MaxCount)
	 * @param MaxCount The maximum count the semaphore can reach
	 */
	FWindowsSemaphore(i32 InitialCount, i32 MaxCount)
		: m_Semaphore(CreateSemaphoreW(nullptr, InitialCount, MaxCount, nullptr))
	{
		OLO_CORE_CHECK_SLOW(m_Semaphore != nullptr);
	}

	~FWindowsSemaphore()
	{
		CloseHandle(m_Semaphore);
	}

	/**
	 * Acquires the semaphore, blocking until the count is positive.
	 * Decrements the count by 1.
	 */
	void Acquire()
	{
		DWORD Res = WaitForSingleObject(m_Semaphore, INFINITE);
		OLO_CORE_CHECK_SLOW(Res == WAIT_OBJECT_0);
	}

	/**
	 * Tries to acquire the semaphore without blocking.
	 * @return true if the semaphore was acquired, false otherwise
	 */
	bool TryAcquire()
	{
		DWORD Res = WaitForSingleObject(m_Semaphore, 0);
		OLO_CORE_CHECK_SLOW(Res == WAIT_OBJECT_0 || Res == WAIT_TIMEOUT);
		return Res == WAIT_OBJECT_0;
	}

	/**
	 * Tries to acquire the semaphore within the given timeout.
	 * @param Timeout The maximum duration to wait
	 * @return true if the semaphore was acquired, false if timed out
	 */
	bool TryAcquireFor(FMonotonicTimeSpan Timeout)
	{
		DWORD TimeoutMs = static_cast<DWORD>(Timeout.ToMilliseconds());
		DWORD Res = WaitForSingleObject(m_Semaphore, TimeoutMs);
		OLO_CORE_CHECK_SLOW(Res == WAIT_OBJECT_0 || Res == WAIT_TIMEOUT);
		return Res == WAIT_OBJECT_0;
	}

	/**
	 * Tries to acquire the semaphore until the given deadline.
	 * @param Deadline The absolute time point to wait until
	 * @return true if the semaphore was acquired, false if timed out
	 */
	bool TryAcquireUntil(FMonotonicTimePoint Deadline)
	{
		FMonotonicTimePoint Now = FMonotonicTimePoint::Now();
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
		OLO_CORE_CHECK_SLOW(bRes);
	}

private:
	HANDLE m_Semaphore;
};

using FSemaphore = FWindowsSemaphore;

#else
// For other platforms using std::counting_semaphore (C++20) or platform-specific APIs

#include <semaphore>
#include <chrono>

class FStdSemaphore
{
public:
	OLO_NONCOPYABLE(FStdSemaphore);

	explicit FStdSemaphore(i32 InitialCount)
		: m_Semaphore(InitialCount)
	{
	}

	FStdSemaphore(i32 InitialCount, i32 /*MaxCount*/)
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
		return m_Semaphore.try_acquire_for(std::chrono::nanoseconds(static_cast<i64>(Timeout.ToNanoseconds())));
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

using FSemaphore = FStdSemaphore;

#endif

} // namespace OloEngine
