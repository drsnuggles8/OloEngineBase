#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MapRange Node - maps a value from one range to another range
	/// Optionally clamps the input to the input range before mapping
	/// Very useful for audio parameter mapping and signal conditioning
	template<typename T>
	class MapRangeNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier InRangeMin_ID = OLO_IDENTIFIER("InRangeMin");
		const Identifier InRangeMax_ID = OLO_IDENTIFIER("InRangeMax");
		const Identifier OutRangeMin_ID = OLO_IDENTIFIER("OutRangeMin");
		const Identifier OutRangeMax_ID = OLO_IDENTIFIER("OutRangeMax");
		const Identifier Clamped_ID = OLO_IDENTIFIER("Clamped");
		const Identifier Result_ID = OLO_IDENTIFIER("Output");

	public:
		MapRangeNode()
		{
			// Register parameters directly
			AddParameter<T>(Input_ID, "Input", T{0});
			AddParameter<T>(InRangeMin_ID, "InRangeMin", T{0});
			AddParameter<T>(InRangeMax_ID, "InRangeMax", T{1});
			AddParameter<T>(OutRangeMin_ID, "OutRangeMin", T{0});
			AddParameter<T>(OutRangeMax_ID, "OutRangeMax", T{1});
			AddParameter<bool>(Clamped_ID, "Clamped", false);
			AddParameter<T>(Result_ID, "Output", T{0});
		}

		virtual ~MapRangeNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const T inputValue = GetParameterValue<T>(Input_ID);
			const T inRangeMin = GetParameterValue<T>(InRangeMin_ID);
			const T inRangeMax = GetParameterValue<T>(InRangeMax_ID);
			const T outRangeMin = GetParameterValue<T>(OutRangeMin_ID);
			const T outRangeMax = GetParameterValue<T>(OutRangeMax_ID);
			const bool clamped = GetParameterValue<bool>(Clamped_ID);

			T result;
			if constexpr (std::is_same_v<T, f32>)
			{
				// Optionally clamp input to input range
				const f32 value = clamped ? glm::clamp(inputValue, inRangeMin, inRangeMax) : inputValue;
				
				// Avoid division by zero
				const f32 inputRange = inRangeMax - inRangeMin;
				if (inputRange == 0.0f)
				{
					result = outRangeMin; // Return minimum output value if input range is zero
				}
				else
				{
					// Calculate normalized position (0.0 to 1.0) in input range
					const f32 t = (value - inRangeMin) / inputRange;
					
					// Map to output range
					result = glm::mix(outRangeMin, outRangeMax, t);
				}
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				// For integer version, use floating point math then convert back
				const i32 value = clamped ? glm::clamp(inputValue, inRangeMin, inRangeMax) : inputValue;
				
				const f32 inputRange = static_cast<f32>(inRangeMax - inRangeMin);
				if (inputRange == 0.0f)
				{
					result = outRangeMin;
				}
				else
				{
					const f32 t = static_cast<f32>(value - inRangeMin) / inputRange;
					const f32 mapped = glm::mix(static_cast<f32>(outRangeMin), static_cast<f32>(outRangeMax), t);
					result = static_cast<i32>(mapped);
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
				return OLO_IDENTIFIER("MapRangeNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MapRangeNodeI32");
			else
				return OLO_IDENTIFIER("MapRangeNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Map Range (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Map Range (i32)";
			else
				return "Map Range";
		}
	};

	// Type aliases for common usage
	using MapRangeNodeF32 = MapRangeNode<f32>;
	using MapRangeNodeI32 = MapRangeNode<i32>;

} // namespace OloEngine::Audio::SoundGraph