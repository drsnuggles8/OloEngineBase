#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Power Node - raises base to the power of exponent (base^exponent)
	/// Supports both f32 and i32 variants using template specialization
	template<typename T>
	class PowerNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Base_ID = OLO_IDENTIFIER("Base");
		const Identifier Exponent_ID = OLO_IDENTIFIER("Exponent");
		const Identifier Result_ID = OLO_IDENTIFIER("Result");

	public:
		PowerNode()
		{
			// Register parameters directly
			AddParameter<T>(Base_ID, "Base", T{2});
			AddParameter<T>(Exponent_ID, "Exponent", T{2});
			AddParameter<T>(Result_ID, "Result", T{0});
		}

		virtual ~PowerNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const T base = GetParameterValue<T>(Base_ID);
			const T exponent = GetParameterValue<T>(Exponent_ID);
			
			T result;
			if constexpr (std::is_same_v<T, f32>)
			{
				result = glm::pow(base, exponent);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				// For integer power, use integer arithmetic for better precision
				result = static_cast<i32>(glm::pow(static_cast<f32>(base), static_cast<f32>(exponent)));
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
				return OLO_IDENTIFIER("PowerNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("PowerNodeI32");
			else
				return OLO_IDENTIFIER("PowerNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Power (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Power (i32)";
			else
				return "Power";
		}
	};

	// Type aliases for common usage
	using PowerNodeF32 = PowerNode<f32>;
	using PowerNodeI32 = PowerNode<i32>;

} // namespace OloEngine::Audio::SoundGraph