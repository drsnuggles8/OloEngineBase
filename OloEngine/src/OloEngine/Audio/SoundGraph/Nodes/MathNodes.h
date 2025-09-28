#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>

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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value = nullptr;
		T* in_Multiplier = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value = nullptr;
		T* in_Denominator = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value1 = nullptr;
		T* in_Value2 = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value = nullptr;
		T* in_MinValue = nullptr;
		T* in_MaxValue = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value = nullptr;
		T* in_FromMin = nullptr;
		T* in_FromMax = nullptr;
		T* in_ToMin = nullptr;
		T* in_ToMax = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			T fromMin = *in_FromMin;
			T fromMax = *in_FromMax;
			T toMin = *in_ToMin;
			T toMax = *in_ToMax;

			// Prevent division by zero
			T fromRange = fromMax - fromMin;
			if constexpr (std::is_floating_point_v<T>)
			{
				if (glm::abs(fromRange) < std::numeric_limits<T>::epsilon())
				{
					out_Out = toMin;
					return;
				}
			}
			else
			{
				if (fromRange == T(0))
				{
					out_Out = toMin;
					return;
				}
			}

			// Linear interpolation: normalize to [0,1] then scale to target range
			// Use wider floating-point type to avoid integer division truncation
			using CalcType = std::conditional_t<std::is_integral_v<T>, double, T>;
			CalcType normalized = (static_cast<CalcType>(*in_Value - fromMin)) / static_cast<CalcType>(fromRange);
			out_Out = static_cast<T>(static_cast<CalcType>(toMin) + normalized * static_cast<CalcType>(toMax - toMin));
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Base = nullptr;
		T* in_Exponent = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
			if constexpr (std::is_floating_point_v<T>)
			{
				out_Out = glm::pow(*in_Base, *in_Exponent);
			}
			else
			{
				// For integer types, use repeated multiplication for positive integer exponents
				T base = *in_Base;
				int exponent = static_cast<int>(*in_Exponent);
				
				if (exponent == 0)
				{
					out_Out = T(1);
				}
				else if (exponent > 0)
				{
					T result = T(1);
					for (int i = 0; i < exponent; ++i)
						result *= base;
					out_Out = result;
				}
				else
				{
					// Negative exponents for integers - return 0 or 1
					out_Out = (glm::abs(base) > T(1)) ? T(0) : T(1);
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

		void RegisterEndpoints();
		void InitializeInputs();

		T* in_Value = nullptr;
		T out_Out{ 0 };

		void Process() final
		{
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