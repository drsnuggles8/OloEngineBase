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

        T* m_Value1 = nullptr;
        T* m_Value2 = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value1 || !m_Value2)
            {
                m_Out = T(0);
                return;
            }
            m_Out = (*m_Value1) + (*m_Value2);
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

        T* m_Value1 = nullptr;
        T* m_Value2 = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value1 || !m_Value2)
            {
                m_Out = T(0);
                return;
            }
            m_Out = (*m_Value1) - (*m_Value2);
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

        T* m_Value = nullptr;
        T* m_Multiplier = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value || !m_Multiplier)
            {
                m_Out = T(0);
                return;
            }
            m_Out = (*m_Value) * (*m_Multiplier);
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

        T* m_Value = nullptr;
        T* m_Denominator = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value || !m_Denominator)
            {
                m_Out = T(0);
                return;
            }

            // Protect against division by zero
            T denominator = *m_Denominator;
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

            m_Out = (*m_Value) / denominator;
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

        T* m_Value1 = nullptr;
        T* m_Value2 = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value1 || !m_Value2)
            {
                m_Out = T(0);
                return;
            }
            m_Out = glm::min(*m_Value1, *m_Value2);
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

        T* m_Value1 = nullptr;
        T* m_Value2 = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value1 || !m_Value2)
            {
                m_Out = T(0);
                return;
            }
            m_Out = glm::max(*m_Value1, *m_Value2);
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

        T* m_Value = nullptr;
        T* m_MinValue = nullptr;
        T* m_MaxValue = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value || !m_MinValue || !m_MaxValue)
            {
                m_Out = T(0);
                return;
            }

            T minVal = *m_MinValue;
            T maxVal = *m_MaxValue;

            // Ensure min <= max
            if (minVal > maxVal)
                std::swap(minVal, maxVal);

            m_Out = glm::clamp(*m_Value, minVal, maxVal);
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

        T* m_Value = nullptr;
        T* m_FromMin = nullptr;
        T* m_FromMax = nullptr;
        T* m_ToMin = nullptr;
        T* m_ToMax = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value || !m_FromMin || !m_FromMax || !m_ToMin || !m_ToMax)
            {
                m_Out = T(0);
                return;
            }

            T fromMin = *m_FromMin;
            T fromMax = *m_FromMax;
            T toMin = *m_ToMin;
            T toMax = *m_ToMax;

            // Use wider floating-point type to avoid integer overflow and division truncation
            using CalcType = std::conditional_t<std::is_integral_v<T>, f64, T>;

            // Cast operands to CalcType before arithmetic to prevent integer overflow
            CalcType fromRange = static_cast<CalcType>(fromMax) - static_cast<CalcType>(fromMin);

            // Prevent division by zero
            if constexpr (std::is_floating_point_v<T>)
            {
                if (glm::abs(fromRange) < std::numeric_limits<CalcType>::epsilon())
                {
                    m_Out = toMin;
                    return;
                }
            }
            else
            {
                if (fromRange == CalcType(0))
                {
                    m_Out = toMin;
                    return;
                }
            }

            // Linear interpolation: normalize to [0,1] then scale to target range
            // Cast all operands to CalcType before arithmetic to prevent overflow
            CalcType valueCast = static_cast<CalcType>(*m_Value);
            CalcType fromMinCast = static_cast<CalcType>(fromMin);
            CalcType toMinCast = static_cast<CalcType>(toMin);
            CalcType toMaxCast = static_cast<CalcType>(toMax);

            CalcType normalized = (valueCast - fromMinCast) / fromRange;
            m_Out = static_cast<T>(toMinCast + normalized * (toMaxCast - toMinCast));
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

        T* m_Base = nullptr;
        T* m_Exponent = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Base || !m_Exponent)
            {
                m_Out = T(0);
                return;
            }

            if constexpr (std::is_floating_point_v<T>)
            {
                m_Out = glm::pow(*m_Base, *m_Exponent);
            }
            else
            {
                // For integer types, use safe fast exponentiation with overflow protection
                T base = *m_Base;
                i32 exponent = static_cast<i32>(*m_Exponent);

                if (exponent == 0)
                {
                    m_Out = T(1);
                }
                else if (exponent < 0)
                {
                    // Handle negative exponents: return 0 for all bases except ±1
                    if (base == T(1))
                    {
                        m_Out = T(1); // 1^(-n) = 1
                    }
                    else if (base == T(-1))
                    {
                        // (-1)^(-n) = (-1)^n, so check parity of exponent
                        m_Out = ((-exponent) % 2 == 0) ? T(1) : T(-1);
                    }
                    else
                    {
                        m_Out = T(0); // All other integer bases with negative exponent
                    }
                }
                else
                {
                    // Positive exponent: use binary exponentiation with overflow checking
                    // Clamp exponent to prevent runaway computation
                    if (constexpr i32 maxExponent = 63; exponent > maxExponent) // Safe limit for most integer types
                    {
                        // For very large exponents, result will be 0 (overflow) or ±1/0 for base ±1/0
                        if (base == T(0))
                            m_Out = T(0);
                        else if (base == T(1))
                            m_Out = T(1);
                        else if (base == T(-1))
                            m_Out = (exponent % 2 == 0) ? T(1) : T(-1);
                        else
                            m_Out = T(0); // Overflow for other bases
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
                                    m_Out = T(0); // Overflow
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
                                        m_Out = currentBase;
                                    else
                                        m_Out = T(0); // Overflow
                                    return;
                                }
                            }
                            currentBase *= currentBase;
                        }
                    }

                    m_Out = result;
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

        T* m_Value = nullptr;
        T m_Out{ 0 };

        void Process(u32 numFrames) final
        {
            // Stateless: output depends only on current input values, so per-call
            // cost is independent of numFrames. The graph still drives this once per
            // sample-step in Phase 1 (numFrames == 1); when block-rate wiring lands
            // in Phase 2, callers will pass the whole block here unchanged.
            (void)numFrames;
            OLO_PROFILE_FUNCTION();

            if (!m_Value)
            {
                m_Out = T(0);
                return;
            }
            m_Out = detail::sat_abs(*m_Value);
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
