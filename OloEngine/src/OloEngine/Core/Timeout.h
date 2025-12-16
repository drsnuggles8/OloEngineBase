// Timeout.h - Utility class for creating timeouts
// Ported from UE 5.7.1 UE::FTimeout

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTime.h"
#include "OloEngine/Core/MonotonicTime.h"
#include <limits>
#include <cmath>

namespace OloEngine
{
	/**
	 * Utility class to create a timeout that will expire at a point in the future.
	 * Example usage:
	 *
	 *	FTimeout TimeoutFromSeconds(0.002);
	 *	while (!TimeoutFromSeconds.IsExpired()) { ... }
	 */
	class FTimeout
	{
	public:
		/** Return true if elapsed time is greater than the initially requested timeout */
		bool IsExpired() const
		{
			// First two cases can skip the slow current time check
			if (WillNeverExpire())
			{
				return false;
			}
			else if (IsAlwaysExpired())
			{
				return true;
			}
			else
			{
				return FPlatformTime::Cycles64() > (StartCycles + TimeoutCycles);
			}
		}

		/** Create a timeout that will never return true for IsExpired */
		static FTimeout Never()
		{
			return FTimeout(FPlatformTime::Cycles64(), NeverExpireCycles);
		}

		/** Returns true if this was created from Never and does not need to be repeatedly checked */
		OLO_FINLINE bool WillNeverExpire() const
		{
			return TimeoutCycles == NeverExpireCycles;
		}

		/** Create a timeout that will always return true for IsExpired */
		static FTimeout AlwaysExpired()
		{
			return FTimeout(FPlatformTime::Cycles64(), 0);
		}

		/** Returns true if this was created from AlwaysExpired and does not need to be repeatedly checked */
		OLO_FINLINE bool IsAlwaysExpired() const
		{
			return TimeoutCycles == 0;
		}

		/** Set this timeout to explicitly expired without recalculating start time */
		void SetToExpired()
		{
			TimeoutCycles = 0;
		}

		// Preferred API for creating and querying using double seconds

		/** Construct a timeout that starts right now and will end after the passed in time in seconds */
		explicit FTimeout(f64 TimeoutSeconds)
			: StartCycles(FPlatformTime::Cycles64())
		{
			SetTimeoutSeconds(TimeoutSeconds);
		}

		/** Construct a timeout that started at the same time as BaseTimeout, but with a new duration */
		explicit FTimeout(const FTimeout& BaseTimeout, f64 TimeoutSeconds)
			: StartCycles(BaseTimeout.StartCycles)
		{
			SetTimeoutSeconds(TimeoutSeconds);
		}

		/** Returns time since the timeout was created, in seconds */
		f64 GetElapsedSeconds() const
		{
			// StartCycles can never be greater than current time as there is no way to construct a timeout starting in the future
			return FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycles);
		}

		/** Returns time left until the timeout expires (which can be negative) in seconds */
		f64 GetRemainingSeconds() const
		{
			if (WillNeverExpire())
			{
				return NeverExpireSeconds;
			}

			// We convert to double separately to avoid underflow on the cycles
			// This could also be done with some branches or treating cycles as signed int64
			return GetTimeoutSeconds() - GetElapsedSeconds();
		}

		/** Returns duration of timeout in seconds */
		f64 GetTimeoutSeconds() const
		{
			return FPlatformTime::ToSeconds64(TimeoutCycles);
		}

		/** Sets the timeout to new value in seconds */
		void SetTimeoutSeconds(f64 TimeoutSeconds)
		{
			if (TimeoutSeconds <= 0.0)
			{
				SetToExpired();
			}
			else
			{
				TimeoutCycles = FPlatformTime::SecondsToCycles64(TimeoutSeconds);
			}
		}

		/** Safely modify the remaining time by adding the delta time in seconds to the timeout */
		void ModifyTimeoutSeconds(f64 DeltaTimeoutSeconds)
		{
			if (IsAlwaysExpired() || WillNeverExpire())
			{
				return;
			}

			if (DeltaTimeoutSeconds >= 0.0)
			{
				TimeoutCycles += FPlatformTime::SecondsToCycles64(DeltaTimeoutSeconds);
			}
			else
			{
				u64 RemovedCycles = FPlatformTime::SecondsToCycles64(-DeltaTimeoutSeconds);
				if (RemovedCycles >= TimeoutCycles)
				{
					SetToExpired();
				}
				else
				{
					TimeoutCycles -= RemovedCycles;
				}
			}
		}

		// API for creating and querying using FMonotonicTimeSpan

		/** Construct a timeout that starts right now and will end after the passed in timespan */
		explicit FTimeout(FMonotonicTimeSpan TimeoutValue)
			: StartCycles(FPlatformTime::Cycles64())
		{
			if (TimeoutValue.IsInfinity())
			{
				TimeoutCycles = NeverExpireCycles;
			}
			else
			{
				SetTimeoutSeconds(TimeoutValue.ToSeconds());
			}
		}

		/** Returns time since the timeout was created, as a timespan */
		FMonotonicTimeSpan GetElapsedTime() const
		{
			return FMonotonicTimeSpan::FromSeconds(GetElapsedSeconds());
		}

		/** Returns time left until the timeout expires (which can be negative) as a timespan */
		FMonotonicTimeSpan GetRemainingTime() const
		{
			if (WillNeverExpire())
			{
				return FMonotonicTimeSpan::Infinity();
			}

			return FMonotonicTimeSpan::FromSeconds(GetRemainingSeconds());
		}

		/** Returns duration of timeout as a timespan */
		FMonotonicTimeSpan GetTimeoutValue() const
		{
			if (WillNeverExpire())
			{
				return FMonotonicTimeSpan::Infinity();
			}

			return FMonotonicTimeSpan::FromSeconds(GetTimeoutSeconds());
		}

		/**
		 * Intended for use in waiting functions, e.g. `FEvent::Wait()`
		 * returns the whole number (rounded up) of remaining milliseconds, clamped into [0, MAX_uint32] range
		 */
		u32 GetRemainingRoundedUpMilliseconds() const
		{
			if (WillNeverExpire())
			{
				return std::numeric_limits<u32>::max();
			}

			f64 RemainingSeconds = GetRemainingSeconds();
			i64 RemainingMsecs = CeilToInt64(RemainingSeconds * 1000.0);
			i64 RemainingMsecsClamped = std::max<i64>(0, std::min<i64>(RemainingMsecs, std::numeric_limits<u32>::max()));
			return static_cast<u32>(RemainingMsecsClamped);
		}

		friend bool operator==(FTimeout Left, FTimeout Right)
		{
			// Timeout cycles need to match which handles differentiating between always and never
			// For normal timeouts, also check the start cycles
			return Left.TimeoutCycles == Right.TimeoutCycles
				&& (Left.WillNeverExpire() || Left.IsAlwaysExpired() || Left.StartCycles == Right.StartCycles);
		}

		friend bool operator!=(FTimeout Left, FTimeout Right)
		{
			return !operator==(Left, Right);
		}

	private:
		FTimeout(u64 StartValue, u64 TimeoutValue)
			: StartCycles(StartValue)
			, TimeoutCycles(TimeoutValue)
		{
		}

		static constexpr u64 NeverExpireCycles = std::numeric_limits<u64>::max();
		static constexpr f64 NeverExpireSeconds = std::numeric_limits<f64>::max();

		// Value of FPlatformTime::Cycles64 at timeout creation, cannot be directly converted to seconds
		u64 StartCycles = 0;

		// Length of timeout, can be converted to seconds as it is relative to StartCycles
		u64 TimeoutCycles = 0;
	};

} // namespace OloEngine
