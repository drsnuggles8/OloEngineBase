#pragma once

#include "OloEngine/Core/Base.h"
#include <chrono>
#include <limits>
#include <type_traits>

namespace OloEngine
{
	//==============================================================================
	/// High-performance random number generator using Linear Congruential Generator (LCG)
	/// Optimized for real-time audio applications where std::random_device may be too slow
	/// 
	/// TODO: Future Algorithm Options (as additional classes, not replacements):
	/// 
	/// PCG32 (Permuted Congruential Generator):
	///   - Excellent statistical properties, passes most randomness tests
	///   - Fast performance comparable to LCG
	///   - 8-byte state (vs 4-byte LCG), very long period (2^64)
	///   - Used by: NumPy, many modern scientific libraries
	///   - Reference: https://www.pcg-random.org/
	/// 
	/// Xorshift32:
	///   - Faster than LCG with better statistical properties
	///   - Same 4-byte state as LCG
	///   - Simple implementation: state ^= state << 13; state ^= state >> 17; state ^= state << 5;
	///   - Used by: V8 JavaScript engine, many game engines
	/// 
	/// Xorshift64/128:
	///   - Even better statistical properties
	///   - Larger state (8/16 bytes) but longer periods
	///   - Excellent for applications needing high-quality randomness
	/// 
	/// Xorshift* / Xorshift+:
	///   - Enhanced versions with better distribution
	///   - Slight performance cost for improved quality
	/// 
	/// SplitMix64:
	///   - Excellent for seeding other generators
	///   - Very fast, good distribution
	///   - Perfect for initializing multiple RNG instances
	/// 
	/// Implementation Strategy:
	///   - Keep FastRandom (LCG) as the lightweight default
	///   - Add FastRandomPCG, FastRandomXorshift classes for specific needs
	///   - Template-based approach: FastRandom<Algorithm> for flexibility
	/// 
	class FastRandom
	{
	private:
		// Template-dependent constant for static_assert in template functions
		template<typename U>
		static inline constexpr bool kAlwaysFalse = false;

	public:
		//==============================================================================
		/// Constructors
		FastRandom() noexcept : m_State(NormalizeSeed(DEFAULT_SEED)) {}
		explicit FastRandom(i32 seed) noexcept : m_State(NormalizeSeed(seed)) {}

		//==============================================================================
		/// Seed management
		void SetSeed(i32 newSeed) noexcept { m_State = NormalizeSeed(newSeed); }
		i32 GetCurrentSeed() const noexcept { return m_State; }

		//==============================================================================
		/// Core random generation
		i32 GetInt32() noexcept
		{
			// LCG: Linear Congruential Generator
			// Formula: (a * seed + c) % m
			// Using constants from Numerical Recipes
			// Use 64-bit arithmetic to avoid 32-bit overflow
			i64 temp = static_cast<i64>(LCG_A) * static_cast<i64>(m_State) + static_cast<i64>(LCG_C);
			m_State = static_cast<i32>(temp % static_cast<i64>(LCG_M));
			return m_State;
		}

		u32 GetUInt32() noexcept 
		{ 
			return static_cast<u32>(GetInt32()); 
		}

		i16 GetInt16() noexcept 
		{ 
			return static_cast<i16>(GetInt32() & 0xFFFF); 
		}

		u16 GetUInt16() noexcept 
		{ 
			return static_cast<u16>(GetInt16() & 0xFFFF); 
		}

		//==============================================================================
		/// Floating point generation
		f64 GetFloat64() noexcept 
		{ 
			// Ensure we get a positive value by taking absolute value and normalizing properly
			// TODO(olbu): Should probably add a GetInt() function so that we only fetch positive values instead of the unsignedValue hack
			const i32 value = GetInt32();
			const u32 unsignedValue = static_cast<u32>(value < 0 ? -value : value);
			return unsignedValue / static_cast<f64>(0x7FFFFFFF); 
		}

		f32 GetFloat32() noexcept 
		{ 
			return static_cast<f32>(GetFloat64()); 
		}

		//==============================================================================
		/// Range-based generation
		f32 GetFloat32InRange(f32 low, f32 high) noexcept
		{
			if (low >= high) return low;
			return low + GetFloat32() * (high - low);
		}

		f64 GetFloat64InRange(f64 low, f64 high) noexcept
		{
			if (low >= high) return low;
			return low + GetFloat64() * (high - low);
		}

		i32 GetInt32InRange(i32 low, i32 high) noexcept
		{
			if (low >= high) return low;
			
			// Compute span using signed 64-bit arithmetic to handle negative values correctly
			const i64 spanSigned = static_cast<i64>(high) - static_cast<i64>(low) + 1;
			OLO_CORE_ASSERT(spanSigned > 0, "FastRandom: span must be positive");
			const u64 span = static_cast<u64>(spanSigned);
			OLO_CORE_ASSERT(span <= static_cast<u64>(LCG_M), "FastRandom: span exceeds generator resolution");
			
			// Use 64-bit arithmetic for rejection sampling to prevent limit becoming zero
			const u32 spanU32 = static_cast<u32>(span);
			const u64 range = static_cast<u64>(LCG_M);  // Full generator domain size
			const u64 limit64 = range - (range % span);
			const u32 limit = static_cast<u32>(limit64);
			
			u32 value;
			do {
				value = GetUInt32();
			} while (value >= limit);
			
			// Use rejection sampling to ensure uniform distribution across the requested range.
			// The 64-bit arithmetic prevents 'limit' from becoming zero when span equals the full generator range.
			return low + static_cast<i32>(value % spanU32);
		}

		//==============================================================================
		/// Generic range functions (template for convenience)
		template<typename T>
		T GetInRange(T low, T high) noexcept
		{
			if constexpr (std::is_same_v<T, f32>)
				return GetFloat32InRange(low, high);
			else if constexpr (std::is_same_v<T, f64>)
				return GetFloat64InRange(low, high);
		else if constexpr (std::is_integral_v<T>)
			return static_cast<T>(GetInt32InRange(static_cast<i32>(low), static_cast<i32>(high)));
		else
			static_assert(kAlwaysFalse<T>, "Unsupported type for GetInRange");
		}

		//==============================================================================
		/// Utility functions
		bool GetBool() noexcept
		{
			return (GetUInt32() & 1) != 0;
		}

		f32 GetNormalizedFloat() noexcept
		{
			return GetFloat32();
		}

		// Get a random value between -1.0f and 1.0f
		f32 GetBipolarFloat() noexcept
		{
			return GetFloat32InRange(-1.0f, 1.0f);
		}

	private:
		i32 m_State;

		// LCG constants from Numerical Recipes
		static constexpr i32 DEFAULT_SEED = 4321;
		static constexpr i32 LCG_M = 2147483647;  // 2^31 - 1
		static constexpr i32 LCG_A = 48271;       // Multiplier
		static constexpr i32 LCG_C = 0;           // Increment

		// Normalize seed to valid range [1, LCG_M - 1] to prevent degenerate states
		static constexpr i32 NormalizeSeed(i32 seed) noexcept
		{
			// Handle zero seed (degenerate case)
			if (seed == 0)
				return DEFAULT_SEED;
			
			// Handle negative seeds by taking absolute value
			if (seed < 0)
			{
				// Prevent overflow when seed is INT32_MIN
				if (seed == std::numeric_limits<i32>::min())
					return DEFAULT_SEED;
				seed = -seed;
			}
			
			// Ensure seed is within valid LCG range [1, LCG_M - 1]
			if (seed >= LCG_M)
				seed = (seed % (LCG_M - 1)) + 1;
			
			return seed;
		}
	};

	//==============================================================================
	/// Utility namespace for random operations
	namespace RandomUtils
	{
		/// Get a random seed from current time (useful for initialization)
		inline i32 GetTimeBasedSeed() noexcept
		{
			const auto now = std::chrono::high_resolution_clock::now();
			const auto timeSinceEpoch = now.time_since_epoch();
			const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(timeSinceEpoch).count();
			
			// Return lower 32 bits as seed
			return static_cast<i32>(microseconds & 0xFFFFFFFF);
		}

		/// Global thread-local random generator (for convenience)
		inline FastRandom& GetGlobalRandom()
		{
			thread_local FastRandom s_GlobalRandom(GetTimeBasedSeed());
			return s_GlobalRandom;
		}

		//==============================================================================
		/// Convenience functions using global generator
		inline f32 Float32() noexcept { return GetGlobalRandom().GetFloat32(); }
		inline f32 Float32(f32 low, f32 high) noexcept { return GetGlobalRandom().GetFloat32InRange(low, high); }
		inline i32 Int32(i32 low, i32 high) noexcept { return GetGlobalRandom().GetInt32InRange(low, high); }
		inline bool Bool() noexcept { return GetGlobalRandom().GetBool(); }
	}

} // namespace OloEngine