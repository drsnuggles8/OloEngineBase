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
    // Addition Node
    //==============================================================================
    template<typename T>
    struct Add : public NodeProcessor
    {
        explicit Add(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue1 = nullptr;
        T* m_InValue2 = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue1 || !m_InValue2)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = (*m_InValue1) + (*m_InValue2);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue1 = nullptr;
        T* m_InValue2 = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue1 || !m_InValue2)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = (*m_InValue1) - (*m_InValue2);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue = nullptr;
        T* m_InMultiplier = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue || !m_InMultiplier)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = (*m_InValue) * (*m_InMultiplier);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue = nullptr;
        T* m_InDenominator = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue || !m_InDenominator)
            {
                m_OutOut = T(0);
                return;
            }

            // Protect against division by zero
            T denominator = *m_InDenominator;
            if constexpr (std::is_floating_point_v<T>)
            {
                if (glm::abs(denominator) < std::numeric_limits<T>::epsilon())
                    denominator = std::copysign(std::numeric_limits<T>::epsilon(), denominator);
            }
            else
            {
                if (denominator == T(0))
                    denominator = T(1);
            }

            m_OutOut = (*m_InValue) / denominator;
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue1 = nullptr;
        T* m_InValue2 = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue1 || !m_InValue2)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = glm::min(*m_InValue1, *m_InValue2);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue1 = nullptr;
        T* m_InValue2 = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue1 || !m_InValue2)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = glm::max(*m_InValue1, *m_InValue2);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue = nullptr;
        T* m_InMinValue = nullptr;
        T* m_InMaxValue = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue || !m_InMinValue || !m_InMaxValue)
            {
                m_OutOut = T(0);
                return;
            }

            T minVal = *m_InMinValue;
            T maxVal = *m_InMaxValue;

            // Ensure min <= max
            if (minVal > maxVal)
                std::swap(minVal, maxVal);

            m_OutOut = glm::clamp(*m_InValue, minVal, maxVal);
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue = nullptr;
        T* m_InFromMin = nullptr;
        T* m_InFromMax = nullptr;
        T* m_InToMin = nullptr;
        T* m_InToMax = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue || !m_InFromMin || !m_InFromMax || !m_InToMin || !m_InToMax)
            {
                m_OutOut = T(0);
                return;
            }

            T fromMin = *m_InFromMin;
            T fromMax = *m_InFromMax;
            T toMin = *m_InToMin;
            T toMax = *m_InToMax;

            // Use wider floating-point type to avoid integer overflow and division truncation
            using CalcType = std::conditional_t<std::is_integral_v<T>, f64, T>;

            // Cast operands to CalcType before arithmetic to prevent integer overflow
            CalcType fromRange = static_cast<CalcType>(fromMax) - static_cast<CalcType>(fromMin);

            // Prevent division by zero
            if constexpr (std::is_floating_point_v<T>)
            {
                if (glm::abs(fromRange) < std::numeric_limits<CalcType>::epsilon())
                {
                    m_OutOut = toMin;
                    return;
                }
            }
            else
            {
                if (fromRange == CalcType(0))
                {
                    m_OutOut = toMin;
                    return;
                }
            }

            // Linear interpolation: normalize to [0,1] then scale to target range
            // Cast all operands to CalcType before arithmetic to prevent overflow
            CalcType valueCast = static_cast<CalcType>(*m_InValue);
            CalcType fromMinCast = static_cast<CalcType>(fromMin);
            CalcType toMinCast = static_cast<CalcType>(toMin);
            CalcType toMaxCast = static_cast<CalcType>(toMax);

            CalcType normalized = (valueCast - fromMinCast) / fromRange;
            m_OutOut = static_cast<T>(toMinCast + normalized * (toMaxCast - toMinCast));
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InBase = nullptr;
        T* m_InExponent = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InBase || !m_InExponent)
            {
                m_OutOut = T(0);
                return;
            }

            if constexpr (std::is_floating_point_v<T>)
            {
                m_OutOut = glm::pow(*m_InBase, *m_InExponent);
            }
            else
            {
                // For integer types, use safe fast exponentiation with overflow protection
                T base = *m_InBase;
                i32 exponent = static_cast<i32>(*m_InExponent);

                if (exponent == 0)
                {
                    m_OutOut = T(1);
                }
                else if (exponent < 0)
                {
                    // Handle negative exponents: return 0 for all bases except ±1
                    if (base == T(1))
                    {
                        m_OutOut = T(1); // 1^(-n) = 1
                    }
                    else if (base == T(-1))
                    {
                        // (-1)^(-n) = (-1)^n, so check parity of exponent
                        m_OutOut = ((-exponent) % 2 == 0) ? T(1) : T(-1);
                    }
                    else
                    {
                        m_OutOut = T(0); // All other integer bases with negative exponent
                    }
                }
                else
                {
                    // Positive exponent: use binary exponentiation with overflow checking
                    // Clamp exponent to prevent runaway computation
                    constexpr i32 maxExponent = 63; // Safe limit for most integer types
                    if (exponent > maxExponent)
                    {
                        // For very large exponents, result will be 0 (overflow) or ±1/0 for base ±1/0
                        if (base == T(0))
                            m_OutOut = T(0);
                        else if (base == T(1))
                            m_OutOut = T(1);
                        else if (base == T(-1))
                            m_OutOut = (exponent % 2 == 0) ? T(1) : T(-1);
                        else
                            m_OutOut = T(0); // Overflow for other bases
                        return;
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
                                {
                                    m_OutOut = T(0); // Overflow
                                    return;
                                }
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
                                        m_OutOut = currentBase;
                                    else
                                        m_OutOut = T(0); // Overflow
                                    return;
                                }
                            }
                            currentBase *= currentBase;
                        }
                    }

                    m_OutOut = result;
                }
            }
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

        void Init() final
        {
            InitializeInputs();
        }

        void RegisterEndpoints()
        {
            EndpointUtilities::RegisterEndpoints(this);
        }

        void InitializeInputs()
        {
            EndpointUtilities::InitializeInputs(this);
        }

        T* m_InValue = nullptr;
        T m_OutOut{ 0 };

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (!m_InValue)
            {
                m_OutOut = T(0);
                return;
            }
            m_OutOut = detail::sat_abs(*m_InValue);
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
