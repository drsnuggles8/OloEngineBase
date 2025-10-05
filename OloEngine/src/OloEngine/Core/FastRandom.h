#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <type_traits>

namespace OloEngine
{
    //==============================================================================
    /// Random Number Generator Algorithm Policies
    /// 
    /// These policy classes implement different RNG algorithms that can be used
    /// with the FastRandom template. Each algorithm provides:
    /// - Next() method returning the raw random output
    /// - Seed() method for initialization
    /// - Static OutputBits constant indicating output bit width
    /// - Static IsHighQuality flag for algorithm quality assessment
    //==============================================================================

    //==============================================================================
    /// Linear Congruential Generator (LCG) - Lightweight, fast
    /// Using constants from Numerical Recipes
    /// Period: 2^31-1, Output: 32-bit
    /// Best for: Simple use cases, minimal memory footprint
    struct LCGAlgorithm
    {
        static constexpr u32 OutputBits = 32;
        static constexpr bool IsHighQuality = false;
        static constexpr const char* Name = "LCG";

        void Seed(u64 seed) noexcept
        {
            // Mix upper and lower 32 bits to preserve entropy from full 64-bit seed
            // This prevents seeds like 0x0000000012345678 and 0xABCDEF0012345678 from colliding
            u32 mixed = static_cast<u32>(seed ^ (seed >> 32));
            
            // Normalize seed to valid range [1, s_LcgM - 1] to prevent degenerate states
            i32 normalizedSeed = static_cast<i32>(mixed);
            
            if (normalizedSeed == 0)
                normalizedSeed = s_DefaultSeed;
            
            if (normalizedSeed < 0)
            {
                if (normalizedSeed == std::numeric_limits<i32>::min())
                    normalizedSeed = s_DefaultSeed;
                else
                    normalizedSeed = -normalizedSeed;
            }
            
            if (normalizedSeed >= s_LcgM)
                normalizedSeed = (normalizedSeed % (s_LcgM - 1)) + 1;
            
            m_State = normalizedSeed;
        }

        u32 Next() noexcept
        {
            // LCG: (a * state + c) % m
            // Use 64-bit arithmetic to avoid 32-bit overflow
            i64 temp = static_cast<i64>(s_LcgA) * static_cast<i64>(m_State) + static_cast<i64>(s_LcgC);
            m_State = static_cast<i32>(temp % static_cast<i64>(s_LcgM));
            return static_cast<u32>(m_State);
        }

        i32 GetState() const noexcept { return m_State; }

    private:
        i32 m_State = s_DefaultSeed;
        
        static constexpr i32 s_DefaultSeed = 4321;
        static constexpr i32 s_LcgM = 2147483647;  // 2^31 - 1
        static constexpr i32 s_LcgA = 48271;       // Multiplier
        static constexpr i32 s_LcgC = 0;           // Increment
    };

    //==============================================================================
    /// PCG32 (Permuted Congruential Generator) - Recommended default
    /// Excellent statistical properties, passes randomness tests
    /// Period: 2^64, Output: 32-bit
    /// Best for: General purpose, high quality with good performance
    /// Reference: https://www.pcg-random.org/
    struct PCG32Algorithm
    {
        static constexpr u32 OutputBits = 32;
        static constexpr bool IsHighQuality = true;
        static constexpr const char* Name = "PCG32";

        void Seed(u64 seed) noexcept
        {
            m_State = seed + s_DefaultInc;
            Next();  // Advance to mix the seed
            m_State += seed;
            Next();
        }

        u32 Next() noexcept
        {
            u64 oldState = m_State;
            // Linear congruential step
            m_State = oldState * s_Multiplier + s_DefaultInc;
            
            // Permutation step (XSH-RR variant)
            u32 xorshifted = static_cast<u32>(((oldState >> 18u) ^ oldState) >> 27u);
            u32 rot = static_cast<u32>(oldState >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
        }

        u64 GetState() const noexcept { return m_State; }

    private:
        u64 m_State = 0x853c49e6748fea9bULL;
        
        static constexpr u64 s_Multiplier = 6364136223846793005ULL;
        static constexpr u64 s_DefaultInc = 1442695040888963407ULL;  // Increment (must be odd)
    };

    //==============================================================================
    /// SplitMix64 - Fast, simple 64-bit generator
    /// Very fast, good distribution, excellent for seeding
    /// Period: 2^64, Output: 64-bit
    /// Best for: Seeding other generators, simple 64-bit needs
    struct SplitMix64Algorithm
    {
        static constexpr u32 OutputBits = 64;
        static constexpr bool IsHighQuality = true;
        static constexpr const char* Name = "SplitMix64";

        void Seed(u64 seed) noexcept
        {
            m_State = seed;
        }

        u64 Next() noexcept
        {
            u64 z = (m_State += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        }

        u64 GetState() const noexcept { return m_State; }

    private:
        u64 m_State = 0x123456789abcdef0ULL;
    };

    //==============================================================================
    /// Xoshiro256++ - High-quality 64-bit generator
    /// Excellent statistical properties, very long period
    /// Period: 2^256-1, Output: 64-bit
    /// Best for: High-quality randomness, parallel streams (with jump)
    struct Xoshiro256ppAlgorithm
    {
        static constexpr u32 OutputBits = 64;
        static constexpr bool IsHighQuality = true;
        static constexpr const char* Name = "Xoshiro256++";

        void Seed(u64 seed) noexcept
        {
            // Use SplitMix64 to initialize the state
            SplitMix64Algorithm splitmix;
            splitmix.Seed(seed);
            m_State[0] = splitmix.Next();
            m_State[1] = splitmix.Next();
            m_State[2] = splitmix.Next();
            m_State[3] = splitmix.Next();
        }

        u64 Next() noexcept
        {
            const u64 result = RotL(m_State[0] + m_State[3], 23) + m_State[0];
            const u64 t = m_State[1] << 17;

            m_State[2] ^= m_State[0];
            m_State[3] ^= m_State[1];
            m_State[1] ^= m_State[2];
            m_State[0] ^= m_State[3];
            m_State[2] ^= t;
            m_State[3] = RotL(m_State[3], 45);

            return result;
        }

        const u64* GetState() const noexcept { return m_State; }

    private:
        u64 m_State[4] = {
            0x123456789abcdef0ULL,
            0xfedcba9876543210ULL,
            0x0fedcba987654321ULL,
            0x10fedcba98765432ULL
        };

        static constexpr u64 RotL(u64 x, int k) noexcept
        {
            return (x << k) | (x >> (64 - k));
        }
    };

    //==============================================================================
    /// FastRandom - Policy-based template for random number generation
    /// 
    /// Template parameter Algorithm can be:
    /// - LCGAlgorithm: Lightweight, minimal state (4 bytes)
    /// - PCG32Algorithm: Recommended default, excellent quality (8 bytes)
    /// - SplitMix64Algorithm: Fast 64-bit generation (8 bytes)
    /// - Xoshiro256ppAlgorithm: Highest quality, parallel streams (32 bytes)
    /// 
    /// Provides complete type support: i8, u8, i16, u16, i32, u32, i64, u64, f32, f64
    /// Optimized for real-time applications where std::random_device may be too slow
    /// 
    template<typename Algorithm = PCG32Algorithm>
    class FastRandom
    {
    public:
        //==============================================================================
        /// Constructors
        FastRandom() noexcept 
        { 
            m_Engine.Seed(s_DefaultSeed);
        }
        
        explicit FastRandom(u64 seed) noexcept 
        { 
            m_Engine.Seed(seed);
        }

        //==============================================================================
        /// Seed management
        
        /// Set the initial seed value
        void SetSeed(u64 newSeed) noexcept 
        { 
            m_Engine.Seed(newSeed);
        }
        
        /// Get the current internal state (varies by algorithm)
        auto GetCurrentState() const noexcept 
        { 
            return m_Engine.GetState();
        }

        //==============================================================================
        /// Core random generation - 8-bit types
        
        i8 GetInt8() noexcept
        {
            OLO_PROFILE_FUNCTION();
            // Use high-order bits for better quality (LCG has poor low-order bits)
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<i8>(NextValue() >> 56);
            else
                return static_cast<i8>(NextValue() >> 24);
        }

        u8 GetUInt8() noexcept
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<u8>(NextValue() >> 56);
            else
                return static_cast<u8>(NextValue() >> 24);
        }

        //==============================================================================
        /// Core random generation - 16-bit types
        
        i16 GetInt16() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<i16>(NextValue() >> 48);
            else
                return static_cast<i16>(NextValue() >> 16);
        }

        u16 GetUInt16() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<u16>(NextValue() >> 48);
            else
                return static_cast<u16>(NextValue() >> 16);
        }

        //==============================================================================
        /// Core random generation - 32-bit types
        
        i32 GetInt32() noexcept
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<i32>(NextValue() >> 32);
            else
                return static_cast<i32>(NextValue());
        }

        u32 GetUInt32() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
                return static_cast<u32>(NextValue() >> 32);
            else
                return static_cast<u32>(NextValue());
        }

        //==============================================================================
        /// Core random generation - 64-bit types
        
        i64 GetInt64() noexcept
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
            {
                return static_cast<i64>(NextValue());
            }
            else
            {
                // Combine two 32-bit calls for 64-bit result
                u64 high = static_cast<u64>(NextValue()) << 32;
                u64 low = static_cast<u64>(NextValue());
                return static_cast<i64>(high | low);
            }
        }

        u64 GetUInt64() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
            {
                return NextValue();
            }
            else
            {
                // Combine two 32-bit calls for 64-bit result
                u64 high = static_cast<u64>(NextValue()) << 32;
                u64 low = static_cast<u64>(NextValue());
                return high | low;
            }
        }

        //==============================================================================
        /// Floating point generation
        
        f64 GetFloat64() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            if constexpr (Algorithm::OutputBits == 64)
            {
                // Use upper 53 bits for double precision mantissa
                constexpr u64 mask = (1ULL << 53) - 1;
                return (NextValue() & mask) / static_cast<f64>(1ULL << 53);
            }
            else
            {
                // For 32-bit algorithms, use full 32-bit value
                return NextValue() / static_cast<f64>(0x100000000ULL);
            }
        }

        f32 GetFloat32() noexcept 
        {
            OLO_PROFILE_FUNCTION();
            // Use upper 24 bits for single precision mantissa
            if constexpr (Algorithm::OutputBits == 64)
            {
                return (NextValue() >> 40) / static_cast<f32>(1 << 24);
            }
            else
            {
                return (NextValue() >> 8) / static_cast<f32>(1 << 24);
            }
        }
        
        //==============================================================================
        /// Range-based generation - 8-bit types
        
        i8 GetInt8InRange(i8 low, i8 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const u32 range = static_cast<u32>(static_cast<i32>(high) - static_cast<i32>(low) + 1);
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % range);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<i8>(value % range);
        }

        u8 GetUInt8InRange(u8 low, u8 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const u32 range = static_cast<u32>(high) - static_cast<u32>(low) + 1;
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % range);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<u8>(value % range);
        }

        //==============================================================================
        /// Range-based generation - 16-bit types
        
        i16 GetInt16InRange(i16 low, i16 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const u32 range = static_cast<u32>(static_cast<i32>(high) - static_cast<i32>(low) + 1);
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % range);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<i16>(value % range);
        }

        u16 GetUInt16InRange(u16 low, u16 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const u32 range = static_cast<u32>(high) - static_cast<u32>(low) + 1;
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % range);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<u16>(value % range);
        }

        //==============================================================================
        /// Range-based generation - 32-bit types
        
        i32 GetInt32InRange(i32 low, i32 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const i64 range = static_cast<i64>(high) - static_cast<i64>(low) + 1;
            const u64 urange = static_cast<u64>(range);
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % urange);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<i32>(value % urange);
        }

        u32 GetUInt32InRange(u32 low, u32 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            const u64 range = static_cast<u64>(high) - static_cast<u64>(low) + 1;
            
            // Use rejection sampling for uniform distribution
            const u64 maxVal = GetMaxValue();
            const u64 limit = maxVal - (maxVal % range);
            
            u64 value;
            do {
                value = NextValue();
            } while (value >= limit);
            
            return low + static_cast<u32>(value % range);
        }

        //==============================================================================
        /// Range-based generation - 64-bit types
        
        i64 GetInt64InRange(i64 low, i64 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            // For 32-bit algorithms, combine two calls
            if constexpr (Algorithm::OutputBits == 32)
            {
                const u64 range = static_cast<u64>(high) - static_cast<u64>(low) + 1;
                u64 value = (static_cast<u64>(NextValue()) << 32) | NextValue();
                
                // Simple modulo (bias is negligible for large ranges)
                return low + static_cast<i64>(value % range);
            }
            else
            {
                const u64 range = static_cast<u64>(high) - static_cast<u64>(low) + 1;
                
                // Use rejection sampling for uniform distribution
                const u64 limit = 0xFFFFFFFFFFFFFFFFULL - (0xFFFFFFFFFFFFFFFFULL % range);
                
                u64 value;
                do {
                    value = NextValue();
                } while (value >= limit);
                
                return low + static_cast<i64>(value % range);
            }
        }

        u64 GetUInt64InRange(u64 low, u64 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low >= high) return low;
            
            // For 32-bit algorithms, combine two calls
            if constexpr (Algorithm::OutputBits == 32)
            {
                const u64 range = high - low + 1;
                u64 value = (static_cast<u64>(NextValue()) << 32) | NextValue();
                
                // Simple modulo (bias is negligible for large ranges)
                return low + (value % range);
            }
            else
            {
                const u64 range = high - low + 1;
                
                // Use rejection sampling for uniform distribution
                const u64 limit = 0xFFFFFFFFFFFFFFFFULL - (0xFFFFFFFFFFFFFFFFULL % range);
                
                u64 value;
                do {
                    value = NextValue();
                } while (value >= limit);
                
                return low + (value % range);
            }
        }

        //==============================================================================
        /// Range-based generation - floating point types
        
        f32 GetFloat32InRange(f32 low, f32 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low > high)
            {
                f32 temp = low;
                low = high;
                high = temp;
            }
            
            if (low == high) return low;
            
            return low + GetFloat32() * (high - low);
        }

        f64 GetFloat64InRange(f64 low, f64 high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            if (low > high)
            {
                f64 temp = low;
                low = high;
                high = temp;
            }
            
            if (low == high) return low;
            
            return low + GetFloat64() * (high - low);
        }

        //==============================================================================
        /// Generic range functions (template for convenience)
        template<typename T>
        T GetInRange(T low, T high) noexcept
        {
            OLO_PROFILE_FUNCTION();
            // Validate range parameters (swap if needed for consistency)
            if (low > high)
            {
                T temp = low;
                low = high;
                high = temp;
            }

            if constexpr (std::is_same_v<T, f32>)
                return GetFloat32InRange(low, high);
            else if constexpr (std::is_same_v<T, f64>)
                return GetFloat64InRange(low, high);
            else if constexpr (std::is_same_v<T, i8>)
                return GetInt8InRange(low, high);
            else if constexpr (std::is_same_v<T, u8>)
                return GetUInt8InRange(low, high);
            else if constexpr (std::is_same_v<T, i16>)
                return GetInt16InRange(low, high);
            else if constexpr (std::is_same_v<T, u16>)
                return GetUInt16InRange(low, high);
            else if constexpr (std::is_same_v<T, i32>)
                return GetInt32InRange(low, high);
            else if constexpr (std::is_same_v<T, u32>)
                return GetUInt32InRange(low, high);
            else if constexpr (std::is_same_v<T, i64>)
                return GetInt64InRange(low, high);
            else if constexpr (std::is_same_v<T, u64>)
                return GetUInt64InRange(low, high);
            else
                static_assert(kAlwaysFalse<T>, "Unsupported type for GetInRange");
        }

        //==============================================================================
        /// Utility functions
        
        bool GetBool() noexcept
        {
            OLO_PROFILE_FUNCTION();
            // Use bit test on random value for better distribution
            return (NextValue() & 1) != 0;
        }

        f32 GetNormalizedFloat() noexcept
        {
            OLO_PROFILE_FUNCTION();
            return GetFloat32();
        }

        // Get a random value between -1.0f and 1.0f
        f32 GetBipolarFloat() noexcept
        {
            OLO_PROFILE_FUNCTION();
            return GetFloat32() * 2.0f - 1.0f;
        }

    private:
        Algorithm m_Engine;
        
        static constexpr u64 s_DefaultSeed = 0x123456789abcdef0ULL;
        
        // Template-dependent constant for static_assert in template functions
        template<typename U>
        static inline constexpr bool kAlwaysFalse = false;

        // Helper to get maximum value based on algorithm output bits
        static constexpr u64 GetMaxValue() noexcept
        {
            if constexpr (Algorithm::OutputBits == 64)
                return 0xFFFFFFFFFFFFFFFFULL;
            else
                return 0xFFFFFFFFULL;
        }

        // Helper to get next value with proper type based on algorithm output
        u64 NextValue() noexcept
        {
            if constexpr (Algorithm::OutputBits == 64)
                return m_Engine.Next();
            else
                return static_cast<u64>(m_Engine.Next());
        }
    };

    //==============================================================================
    /// Type aliases for common algorithm choices
    using FastRandomLCG = FastRandom<LCGAlgorithm>;
    using FastRandomPCG = FastRandom<PCG32Algorithm>;
    using FastRandomSplitMix = FastRandom<SplitMix64Algorithm>;
    using FastRandomXoshiro = FastRandom<Xoshiro256ppAlgorithm>;

    //==============================================================================
    /// Utility namespace for random operations
    namespace RandomUtils
    {
        /// Get a random seed from current time (useful for initialization)
        inline u64 GetTimeBasedSeed() noexcept
        {
            const auto now = std::chrono::high_resolution_clock::now();
            const auto timeSinceEpoch = now.time_since_epoch();
            const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(timeSinceEpoch).count();
            
            return static_cast<u64>(nanoseconds);
        }

        /// Global thread-local random generator (using PCG32 for quality)
        inline FastRandomPCG& GetGlobalRandom()
        {
            thread_local FastRandomPCG s_GlobalRandom(GetTimeBasedSeed());
            return s_GlobalRandom;
        }

        //==============================================================================
        /// Convenience functions using global generator - 8-bit types
        inline i8 Int8(i8 low, i8 high) noexcept { return GetGlobalRandom().GetInt8InRange(low, high); }
        inline u8 UInt8(u8 low, u8 high) noexcept { return GetGlobalRandom().GetUInt8InRange(low, high); }
        
        /// Convenience functions using global generator - 16-bit types
        inline i16 Int16(i16 low, i16 high) noexcept { return GetGlobalRandom().GetInt16InRange(low, high); }
        inline u16 UInt16(u16 low, u16 high) noexcept { return GetGlobalRandom().GetUInt16InRange(low, high); }
        
        /// Convenience functions using global generator - 32-bit types
        inline i32 Int32(i32 low, i32 high) noexcept { return GetGlobalRandom().GetInt32InRange(low, high); }
        inline u32 UInt32(u32 low, u32 high) noexcept { return GetGlobalRandom().GetUInt32InRange(low, high); }
        
        /// Convenience functions using global generator - 64-bit types
        inline i64 Int64(i64 low, i64 high) noexcept { return GetGlobalRandom().GetInt64InRange(low, high); }
        inline u64 UInt64(u64 low, u64 high) noexcept { return GetGlobalRandom().GetUInt64InRange(low, high); }
        
        /// Convenience functions using global generator - floating point
        inline f32 Float32() noexcept { return GetGlobalRandom().GetFloat32(); }
        inline f32 Float32(f32 low, f32 high) noexcept { return GetGlobalRandom().GetFloat32InRange(low, high); }
        inline f64 Float64() noexcept { return GetGlobalRandom().GetFloat64(); }
        inline f64 Float64(f64 low, f64 high) noexcept { return GetGlobalRandom().GetFloat64InRange(low, high); }
        
        /// Convenience functions using global generator - utility
        inline bool Bool() noexcept { return GetGlobalRandom().GetBool(); }
    }

} // namespace OloEngine