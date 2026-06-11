#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/log_base.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    // Math utilities
    //==============================================================================
    namespace detail
    {
        /// Saturating absolute value that handles INT_MIN safely
        template<typename T>
        constexpr T sat_abs(T value) noexcept
        {
            if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            {
                // For signed integers, handle INT_MIN overflow case
                if (value == std::numeric_limits<T>::min())
                    return std::numeric_limits<T>::max();
                return value < T(0) ? -value : value;
            }
            else
            {
                // For unsigned types and floating point, use standard abs
                return glm::abs(value);
            }
        }
    } // namespace detail

    //==============================================================================
    // Member type selection for the math node templates.
    //
    // f32 math nodes are audio-rate: inputs are AudioBufferRef (per-frame samples
    // from a producer buffer, or a scalar broadcast) and the output is a block
    // AudioBuffer — an oscillator * envelope chain stays sample-accurate.
    // Integer math nodes are control-rate: scalar refs in, scalar member out,
    // computed once per block.
    //==============================================================================
    template<typename T>
    using MathInRef = std::conditional_t<std::is_same_v<T, f32>, AudioBufferRef, ValueRef<T>>;

    template<typename T>
    using MathOut = std::conditional_t<std::is_same_v<T, f32>, AudioBuffer, T>;

    //==============================================================================
    // Addition Node
    //==============================================================================
    template<typename T>
    struct Add : public NodeProcessor
    {
        explicit Add(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value1;
        MathInRef<T> m_Value2;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = m_Value1.Sample(frame) + m_Value2.Sample(frame);
            }
            else
            {
                (void)numFrames;
                m_Out = m_Value1.Get() + m_Value2.Get();
            }
        }
    };

    //==============================================================================
    // Subtraction Node
    //==============================================================================
    template<typename T>
    struct Subtract : public NodeProcessor
    {
        explicit Subtract(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value1;
        MathInRef<T> m_Value2;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = m_Value1.Sample(frame) - m_Value2.Sample(frame);
            }
            else
            {
                (void)numFrames;
                m_Out = m_Value1.Get() - m_Value2.Get();
            }
        }
    };

    //==============================================================================
    // Multiplication Node
    //==============================================================================
    template<typename T>
    struct Multiply : public NodeProcessor
    {
        explicit Multiply(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value;
        MathInRef<T> m_Multiplier;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = m_Value.Sample(frame) * m_Multiplier.Sample(frame);
            }
            else
            {
                (void)numFrames;
                m_Out = m_Value.Get() * m_Multiplier.Get();
            }
        }
    };

    //==============================================================================
    // Division Node
    //==============================================================================
    template<typename T>
    struct Divide : public NodeProcessor
    {
        explicit Divide(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value;
        MathInRef<T> m_Denominator;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                {
                    f32 denominator = m_Denominator.Sample(frame);
                    if (glm::abs(denominator) < std::numeric_limits<f32>::epsilon())
                        denominator = std::copysign(std::numeric_limits<f32>::epsilon(), denominator);
                    out[frame] = m_Value.Sample(frame) / denominator;
                }
            }
            else
            {
                (void)numFrames;
                T denominator = m_Denominator.Get();
                if (denominator == T(0))
                    denominator = T(1);
                m_Out = m_Value.Get() / denominator;
            }
        }
    };

    //==============================================================================
    // Minimum Node
    //==============================================================================
    template<typename T>
    struct Min : public NodeProcessor
    {
        explicit Min(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value1;
        MathInRef<T> m_Value2;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = glm::min(m_Value1.Sample(frame), m_Value2.Sample(frame));
            }
            else
            {
                (void)numFrames;
                m_Out = glm::min(m_Value1.Get(), m_Value2.Get());
            }
        }
    };

    //==============================================================================
    // Maximum Node
    //==============================================================================
    template<typename T>
    struct Max : public NodeProcessor
    {
        explicit Max(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value1;
        MathInRef<T> m_Value2;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = glm::max(m_Value1.Sample(frame), m_Value2.Sample(frame));
            }
            else
            {
                (void)numFrames;
                m_Out = glm::max(m_Value1.Get(), m_Value2.Get());
            }
        }
    };

    //==============================================================================
    // Clamp Node
    //==============================================================================
    template<typename T>
    struct Clamp : public NodeProcessor
    {
        explicit Clamp(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value;
        MathInRef<T> m_MinValue;
        MathInRef<T> m_MaxValue;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                {
                    f32 minVal = m_MinValue.Sample(frame);
                    f32 maxVal = m_MaxValue.Sample(frame);
                    if (minVal > maxVal)
                        std::swap(minVal, maxVal);
                    out[frame] = glm::clamp(m_Value.Sample(frame), minVal, maxVal);
                }
            }
            else
            {
                (void)numFrames;
                T minVal = m_MinValue.Get();
                T maxVal = m_MaxValue.Get();
                if (minVal > maxVal)
                    std::swap(minVal, maxVal);
                m_Out = glm::clamp(m_Value.Get(), minVal, maxVal);
            }
        }
    };

    //==============================================================================
    // Map Range Node - Linear mapping from one range to another
    //==============================================================================
    template<typename T>
    struct MapRange : public NodeProcessor
    {
        explicit MapRange(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value;
        MathInRef<T> m_FromMin;
        MathInRef<T> m_FromMax;
        MathInRef<T> m_ToMin;
        MathInRef<T> m_ToMax;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = MapOne(m_Value.Sample(frame), m_FromMin.Sample(frame), m_FromMax.Sample(frame),
                                        m_ToMin.Sample(frame), m_ToMax.Sample(frame));
            }
            else
            {
                (void)numFrames;
                m_Out = MapOne(m_Value.Get(), m_FromMin.Get(), m_FromMax.Get(), m_ToMin.Get(), m_ToMax.Get());
            }
        }

      private:
        static T MapOne(T value, T fromMin, T fromMax, T toMin, T toMax) noexcept
        {
            // Use wider floating-point type to avoid integer overflow and division truncation
            using CalcType = std::conditional_t<std::is_integral_v<T>, f64, T>;

            // Cast operands to CalcType before arithmetic to prevent integer overflow
            CalcType fromRange = static_cast<CalcType>(fromMax) - static_cast<CalcType>(fromMin);

            // Prevent division by zero
            if constexpr (std::is_floating_point_v<T>)
            {
                if (glm::abs(fromRange) < std::numeric_limits<CalcType>::epsilon())
                    return toMin;
            }
            else
            {
                if (fromRange == CalcType(0))
                    return toMin;
            }

            // Linear interpolation: normalize to [0,1] then scale to target range
            CalcType normalized = (static_cast<CalcType>(value) - static_cast<CalcType>(fromMin)) / fromRange;
            return static_cast<T>(static_cast<CalcType>(toMin) +
                                  normalized * (static_cast<CalcType>(toMax) - static_cast<CalcType>(toMin)));
        }
    };

    //==============================================================================
    // Power Node
    //==============================================================================
    template<typename T>
    struct Power : public NodeProcessor
    {
        explicit Power(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Base;
        MathInRef<T> m_Exponent;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = glm::pow(m_Base.Sample(frame), m_Exponent.Sample(frame));
            }
            else
            {
                (void)numFrames;
                m_Out = IntegerPower(m_Base.Get(), static_cast<i32>(m_Exponent.Get()));
            }
        }

      private:
        // Safe fast exponentiation with overflow protection for integer types
        static T IntegerPower(T base, i32 exponent) noexcept
        {
            if (exponent == 0)
                return T(1);

            if (exponent < 0)
            {
                // Handle negative exponents: return 0 for all bases except ±1
                if (base == T(1))
                    return T(1); // 1^(-n) = 1
                if (base == T(-1))
                {
                    // (-1)^(-n) = (-1)^n, so check parity of exponent
                    return ((-exponent) % 2 == 0) ? T(1) : T(-1);
                }
                return T(0); // All other integer bases with negative exponent
            }

            // Positive exponent: use binary exponentiation with overflow checking
            // Clamp exponent to prevent runaway computation
            if (constexpr i32 maxExponent = 63; exponent > maxExponent) // Safe limit for most integer types
            {
                // For very large exponents, result will be 0 (overflow) or ±1/0 for base ±1/0
                if (base == T(0))
                    return T(0);
                if (base == T(1))
                    return T(1);
                if (base == T(-1))
                    return (exponent % 2 == 0) ? T(1) : T(-1);
                return T(0); // Overflow for other bases
            }

            // Binary exponentiation with overflow protection
            T result = T(1);
            T currentBase = base;
            u32 exp = static_cast<u32>(exponent);

            while (exp > 0)
            {
                if (exp & 1) // If exponent is odd
                {
                    // Check for overflow before multiplication
                    if (result != T(0) && currentBase != T(0))
                    {
                        T maxDiv = std::numeric_limits<T>::max() / detail::sat_abs(currentBase);
                        if (detail::sat_abs(result) > maxDiv)
                            return T(0); // Overflow
                    }
                    result *= currentBase;
                }

                exp >>= 1; // Divide exponent by 2
                if (exp > 0)
                {
                    // Check for overflow before squaring base
                    if (currentBase != T(0))
                    {
                        T maxSqrt = static_cast<T>(std::sqrt(static_cast<f64>(std::numeric_limits<T>::max())));
                        if (detail::sat_abs(currentBase) > maxSqrt)
                        {
                            if (result == T(1)) // Only first iteration, so base^1 is still valid
                                return currentBase;
                            return T(0); // Overflow
                        }
                    }
                    currentBase *= currentBase;
                }
            }

            return result;
        }
    };

    //==============================================================================
    // Absolute Value Node
    //==============================================================================
    template<typename T>
    struct Abs : public NodeProcessor
    {
        explicit Abs(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void RegisterEndpoints(); // defined out-of-line in NodeTypes.cpp where DESCRIBE_NODE specializations are visible

        MathInRef<T> m_Value;
        MathOut<T> m_Out{};

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if constexpr (std::is_same_v<T, f32>)
            {
                f32* out = m_Out.Data();
                for (u32 frame = 0; frame < numFrames; ++frame)
                    out[frame] = detail::sat_abs(m_Value.Sample(frame));
            }
            else
            {
                (void)numFrames;
                m_Out = detail::sat_abs(m_Value.Get());
            }
        }
    };

    //==============================================================================
    // Type aliases for common instantiations
    //==============================================================================
    using AddFloat = Add<f32>;
    using SubtractFloat = Subtract<f32>;
    using MultiplyFloat = Multiply<f32>;
    using DivideFloat = Divide<f32>;
    using MinFloat = Min<f32>;
    using MaxFloat = Max<f32>;
    using ClampFloat = Clamp<f32>;
    using MapRangeFloat = MapRange<f32>;
    using PowerFloat = Power<f32>;
    using AbsFloat = Abs<f32>;

    using AddInt = Add<i32>;
    using SubtractInt = Subtract<i32>;
    using MultiplyInt = Multiply<i32>;
    using DivideInt = Divide<i32>;
    using MinInt = Min<i32>;
    using MaxInt = Max<i32>;
    using ClampInt = Clamp<i32>;
    using MapRangeInt = MapRange<i32>;
    using PowerInt = Power<i32>;
    using AbsInt = Abs<i32>;
} // namespace OloEngine::Audio::SoundGraph
