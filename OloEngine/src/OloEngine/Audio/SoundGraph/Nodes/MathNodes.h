#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <limits>
#include <type_traits>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/log_base.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
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

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value1 || !in_Value2)
			{
				out_Out = T(0);
				return;
			}
			out_Out = (*in_Value1) + (*in_Value2);
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

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value1 || !in_Value2)
			{
				out_Out = T(0);
				return;
			}
			out_Out = (*in_Value1) - (*in_Value2);
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

		T* in_Value = nullptr;
		T* in_Multiplier = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value || !in_Multiplier)
			{
				out_Out = T(0);
				return;
			}
			out_Out = (*in_Value) * (*in_Multiplier);
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

		T* in_Value = nullptr;
		T* in_Denominator = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value || !in_Denominator)
			{
				out_Out = T(0);
				return;
			}
			
			// Protect against division by zero
			T denominator = *in_Denominator;
			if constexpr (std::is_floating_point_v<T>)
			{
				if (glm::abs(denominator) < std::numeric_limits<T>::epsilon())
					denominator = std::numeric_limits<T>::epsilon();
			}
			else
			{
				if (denominator == T(0))
					denominator = T(1);
			}

			out_Out = (*in_Value) / denominator;
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

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value1 || !in_Value2)
			{
				out_Out = T(0);
				return;
			}
			out_Out = glm::min(*in_Value1, *in_Value2);
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

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value1 || !in_Value2)
			{
				out_Out = T(0);
				return;
			}
			out_Out = glm::max(*in_Value1, *in_Value2);
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

		T* in_Value = nullptr;
		T* in_MinValue = nullptr;
		T* in_MaxValue = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value || !in_MinValue || !in_MaxValue)
			{
				out_Out = T(0);
				return;
			}
			
			T minVal = *in_MinValue;
			T maxVal = *in_MaxValue;
			
			// Ensure min <= max
			if (minVal > maxVal)
				std::swap(minVal, maxVal);

			out_Out = glm::clamp(*in_Value, minVal, maxVal);
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

		T* in_Value = nullptr;
		T* in_FromMin = nullptr;
		T* in_FromMax = nullptr;
		T* in_ToMin = nullptr;
		T* in_ToMax = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value || !in_FromMin || !in_FromMax || !in_ToMin || !in_ToMax)
			{
				out_Out = T(0);
				return;
			}
			
			T fromMin = *in_FromMin;
			T fromMax = *in_FromMax;
			T toMin = *in_ToMin;
			T toMax = *in_ToMax;

			// Use wider floating-point type to avoid integer overflow and division truncation
			using CalcType = std::conditional_t<std::is_integral_v<T>, double, T>;
			
			// Cast operands to CalcType before arithmetic to prevent integer overflow
			CalcType fromRange = static_cast<CalcType>(fromMax) - static_cast<CalcType>(fromMin);
			
			// Prevent division by zero
			if constexpr (std::is_floating_point_v<T>)
			{
				if (glm::abs(fromRange) < std::numeric_limits<CalcType>::epsilon())
				{
					out_Out = toMin;
					return;
				}
			}
			else
			{
				if (fromRange == CalcType(0))
				{
					out_Out = toMin;
					return;
				}
			}

			// Linear interpolation: normalize to [0,1] then scale to target range
			// Cast all operands to CalcType before arithmetic to prevent overflow
			CalcType valueCast = static_cast<CalcType>(*in_Value);
			CalcType fromMinCast = static_cast<CalcType>(fromMin);
			CalcType toMinCast = static_cast<CalcType>(toMin);
			CalcType toMaxCast = static_cast<CalcType>(toMax);
			
			CalcType normalized = (valueCast - fromMinCast) / fromRange;
			out_Out = static_cast<T>(toMinCast + normalized * (toMaxCast - toMinCast));
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

		T* in_Base = nullptr;
		T* in_Exponent = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Base || !in_Exponent)
			{
				out_Out = T(0);
				return;
			}
			
			if constexpr (std::is_floating_point_v<T>)
			{
				out_Out = glm::pow(*in_Base, *in_Exponent);
			}
			else
			{
				// For integer types, use safe fast exponentiation with overflow protection
				T base = *in_Base;
				int exponent = static_cast<int>(*in_Exponent);
				
				if (exponent == 0)
				{
					out_Out = T(1);
				}
				else if (exponent < 0)
				{
					// Handle negative exponents: return 0 for all bases except ±1
					if (base == T(1))
					{
						out_Out = T(1); // 1^(-n) = 1
					}
					else if (base == T(-1))
					{
						// (-1)^(-n) = (-1)^n, so check parity of exponent
						out_Out = ((-exponent) % 2 == 0) ? T(1) : T(-1);
					}
					else
					{
						out_Out = T(0); // All other integer bases with negative exponent
					}
				}
				else
				{
					// Positive exponent: use binary exponentiation with overflow checking
					// Clamp exponent to prevent runaway computation
					constexpr int maxExponent = 63; // Safe limit for most integer types
					if (exponent > maxExponent)
					{
						// For very large exponents, result will be 0 (overflow) or ±1/0 for base ±1/0
						if (base == T(0))
							out_Out = T(0);
						else if (base == T(1))
							out_Out = T(1);
						else if (base == T(-1))
							out_Out = (exponent % 2 == 0) ? T(1) : T(-1);
						else
							out_Out = T(0); // Overflow for other bases
						return;
					}
					
					// Binary exponentiation with overflow protection
					T result = T(1);
					T currentBase = base;
					unsigned int exp = static_cast<unsigned int>(exponent);
					
					while (exp > 0)
					{
						if (exp & 1) // If exponent is odd
						{
							// Check for overflow before multiplication
							if (result != T(0) && currentBase != T(0))
							{
								T maxDiv = std::numeric_limits<T>::max() / glm::abs(currentBase);
								if (glm::abs(result) > maxDiv)
								{
									out_Out = T(0); // Overflow
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
								T maxSqrt = static_cast<T>(std::sqrt(static_cast<double>(std::numeric_limits<T>::max())));
								if (glm::abs(currentBase) > maxSqrt)
								{
									if (result == T(1)) // Only first iteration, so base^1 is still valid
										out_Out = currentBase;
									else
										out_Out = T(0); // Overflow
									return;
								}
							}
							currentBase *= currentBase;
						}
					}
					
					out_Out = result;
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

		T* in_Value = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if (!in_Value)
			{
				out_Out = T(0);
				return;
			}
			out_Out = glm::abs(*in_Value);
		}
	};

	//==============================================================================
	// Type aliases for common instantiations
	//==============================================================================
	using AddFloat = Add<float>;
	using SubtractFloat = Subtract<float>;
	using MultiplyFloat = Multiply<float>;
	using DivideFloat = Divide<float>;
	using MinFloat = Min<float>;
	using MaxFloat = Max<float>;
	using ClampFloat = Clamp<float>;
	using MapRangeFloat = MapRange<float>;
	using PowerFloat = Power<float>;
	using AbsFloat = Abs<float>;

	using AddInt = Add<int>;
	using SubtractInt = Subtract<int>;
	using MultiplyInt = Multiply<int>;
	using DivideInt = Divide<int>;
	using MinInt = Min<int>;
	using MaxInt = Max<int>;
	using ClampInt = Clamp<int>;
	using MapRangeInt = MapRange<int>;
	using PowerInt = Power<int>;
	using AbsInt = Abs<int>;
}

#undef DECLARE_ID