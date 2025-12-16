// PlatformTime.h - Platform-independent high-resolution time functions
// Ported from UE5.7 FPlatformTime

#pragma once

#include "OloEngine/Core/Base.h"
#include <chrono>

namespace OloEngine
{
	/**
	 * Platform-independent time utilities.
	 */
	struct FPlatformTime
	{
		/**
		 * Get seconds since application start or arbitrary epoch.
		 * Uses steady_clock for monotonic behavior.
		 */
		static f64 Seconds()
		{
			static const auto StartTime = std::chrono::steady_clock::now();
			const auto Now = std::chrono::steady_clock::now();
			const auto Duration = std::chrono::duration<f64>(Now - StartTime);
			return Duration.count();
		}

		/**
		 * Get current CPU cycle count (32-bit version for quick randomness).
		 */
		static u32 Cycles()
		{
			return static_cast<u32>(Cycles64());
		}

		/**
		 * Get current CPU cycle count.
		 */
		static u64 Cycles64()
		{
#if defined(_MSC_VER)
			return __rdtsc();
#elif defined(__GNUC__) || defined(__clang__)
	#if defined(__x86_64__) || defined(__i386__)
			u32 Lo, Hi;
			__asm__ __volatile__("rdtsc" : "=a"(Lo), "=d"(Hi));
			return (static_cast<u64>(Hi) << 32) | Lo;
	#elif defined(__aarch64__)
			u64 Val;
			__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(Val));
			return Val;
	#else
			return 0;
	#endif
#else
			return 0;
#endif
		}

		/**
		 * Convert cycles to seconds.
		 */
		static f64 ToSeconds64(u64 Cycles)
		{
			// Approximate CPU frequency - in reality this would be calibrated
			// For now, using a typical 3 GHz CPU as baseline
			static const f64 s_SecondsPerCycle = 1.0 / 3000000000.0;
			return static_cast<f64>(Cycles) * s_SecondsPerCycle;
		}

		/**
		 * Convert seconds to cycles.
		 */
		static u64 SecondsToCycles64(f64 Seconds)
		{
			// Approximate CPU frequency - in reality this would be calibrated
			// For now, using a typical 3 GHz CPU as baseline
			static const f64 s_SecondsPerCycle = 1.0 / 3000000000.0;
			return static_cast<u64>(Seconds / s_SecondsPerCycle);
		}
	};

	/**
	 * Ceil a floating-point number to int64.
	 */
	inline i64 CeilToInt64(f64 Value)
	{
		return static_cast<i64>(std::ceil(Value));
	}

} // namespace OloEngine
