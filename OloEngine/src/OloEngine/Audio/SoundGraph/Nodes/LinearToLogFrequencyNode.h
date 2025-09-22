#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// LinearToLogFrequency Node - converts linear values to logarithmic frequency scale
	/// Essential for audio applications where frequency perception is logarithmic
	/// Maps a linear input range to a logarithmic frequency range (e.g., for frequency controls)
	class LinearToLogFrequencyNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Value_ID = OLO_IDENTIFIER("Value");
		const Identifier MinValue_ID = OLO_IDENTIFIER("MinValue");
		const Identifier MaxValue_ID = OLO_IDENTIFIER("MaxValue");
		const Identifier MinFrequency_ID = OLO_IDENTIFIER("MinFrequency");
		const Identifier MaxFrequency_ID = OLO_IDENTIFIER("MaxFrequency");
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");

	public:
		LinearToLogFrequencyNode()
		{
			// Register parameters directly
			AddParameter<f32>(Value_ID, "Value", 0.5f);
			AddParameter<f32>(MinValue_ID, "MinValue", 0.0f);
			AddParameter<f32>(MaxValue_ID, "MaxValue", 1.0f);
			AddParameter<f32>(MinFrequency_ID, "MinFrequency", 20.0f);   // 20Hz - low end of human hearing
			AddParameter<f32>(MaxFrequency_ID, "MaxFrequency", 20000.0f); // 20kHz - high end of human hearing
			AddParameter<f32>(Frequency_ID, "Frequency", 1000.0f);       // 1kHz default output
		}

		virtual ~LinearToLogFrequencyNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 value = GetParameterValue<f32>(Value_ID);
			const f32 minValue = GetParameterValue<f32>(MinValue_ID);
			const f32 maxValue = GetParameterValue<f32>(MaxValue_ID);
			const f32 minFrequency = GetParameterValue<f32>(MinFrequency_ID);
			const f32 maxFrequency = GetParameterValue<f32>(MaxFrequency_ID);

			f32 frequency;
			
			// Avoid division by zero and invalid logarithms
			if (maxValue == minValue || minFrequency <= 0.0f || maxFrequency <= 0.0f || minFrequency >= maxFrequency)
			{
				frequency = minFrequency; // Safe fallback
			}
			else
			{
				// Normalize the input value to 0-1 range
				const f32 normalizedValue = glm::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
				
				// Calculate the octave range
				const f32 octaveRange = glm::log2(maxFrequency / minFrequency);
				
				// Map to logarithmic frequency scale
				frequency = glm::exp2(normalizedValue * octaveRange) * minFrequency;
			}

			// Set output parameter
			SetParameterValue(Frequency_ID, frequency);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = frequency;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("LinearToLogFrequencyNode");
		}

		const char* GetDisplayName() const override
		{
			return "Linear to Log Frequency";
		}
	};

} // namespace OloEngine::Audio::SoundGraph