#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/log_base.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Log Node - calculates logarithm of value with specified base (log_base(value))
	/// Supports both f32 and i32 variants using template specialization
	template<typename T>
	class LogNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Base_ID = OLO_IDENTIFIER("Base");
		const Identifier Value_ID = OLO_IDENTIFIER("Value");
		const Identifier Result_ID = OLO_IDENTIFIER("Result");

	public:
		LogNode()
		{
			// Register parameters directly
			AddParameter<T>(Base_ID, "Base", T{10}); // Default to base 10
			AddParameter<T>(Value_ID, "Value", T{1});
			AddParameter<T>(Result_ID, "Result", T{0});
		}

		virtual ~LogNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const T base = GetParameterValue<T>(Base_ID);
			const T value = GetParameterValue<T>(Value_ID);
			
			T result;
			if constexpr (std::is_same_v<T, f32>)
			{
				// Prevent logarithm of zero or negative numbers
				if (value <= 0.0f || base <= 0.0f || base == 1.0f)
				{
					result = 0.0f; // Safe fallback value
				}
				else
				{
					result = glm::log(value, base);
				}
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				// For integer log, convert to float, calculate, then back to int
				if (value <= 0 || base <= 0 || base == 1)
				{
					result = 0; // Safe fallback value
				}
				else
				{
					result = static_cast<i32>(glm::log(static_cast<f32>(value), static_cast<f32>(base)));
				}
			}

			// Set output parameter
			SetParameterValue(Result_ID, result);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = static_cast<f32>(result);
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LogNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LogNodeI32");
			else
				return OLO_IDENTIFIER("LogNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Log (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Log (i32)";
			else
				return "Log";
		}
	};

	// Type aliases for common usage
	using LogNodeF32 = LogNode<f32>;
	using LogNodeI32 = LogNode<i32>;

} // namespace OloEngine::Audio::SoundGraph