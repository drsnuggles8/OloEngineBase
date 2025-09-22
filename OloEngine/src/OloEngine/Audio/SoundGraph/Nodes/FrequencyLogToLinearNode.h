#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// FrequencyLogToLinear Node - converts logarithmic frequency values to linear scale
	/// Inverse operation of LinearToLogFrequency, essential for frequency analysis
	/// Maps a logarithmic frequency input to a linear output range
	class FrequencyLogToLinearNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier MinFrequency_ID = OLO_IDENTIFIER("MinFrequency");
		const Identifier MaxFrequency_ID = OLO_IDENTIFIER("MaxFrequency");
		const Identifier MinValue_ID = OLO_IDENTIFIER("MinValue");
		const Identifier MaxValue_ID = OLO_IDENTIFIER("MaxValue");
		const Identifier Value_ID = OLO_IDENTIFIER("Value");

	public:
		FrequencyLogToLinearNode()
		{
			// Register parameters directly
			AddParameter<f32>(Frequency_ID, "Frequency", 1000.0f);     // 1kHz default input
			AddParameter<f32>(MinFrequency_ID, "MinFrequency", 20.0f);   // 20Hz - low end of human hearing
			AddParameter<f32>(MaxFrequency_ID, "MaxFrequency", 20000.0f); // 20kHz - high end of human hearing
			AddParameter<f32>(MinValue_ID, "MinValue", 0.0f);
			AddParameter<f32>(MaxValue_ID, "MaxValue", 1.0f);
			AddParameter<f32>(Value_ID, "Value", 0.5f);                // Default linear output
		}

		virtual ~FrequencyLogToLinearNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 frequency = GetParameterValue<f32>(Frequency_ID);
			const f32 minFrequency = GetParameterValue<f32>(MinFrequency_ID);
			const f32 maxFrequency = GetParameterValue<f32>(MaxFrequency_ID);
			const f32 minValue = GetParameterValue<f32>(MinValue_ID);
			const f32 maxValue = GetParameterValue<f32>(MaxValue_ID);

			f32 value;
			
			// Avoid division by zero and invalid logarithms
			if (minFrequency <= 0.0f || maxFrequency <= 0.0f || minFrequency >= maxFrequency || frequency <= 0.0f)
			{
				value = minValue; // Safe fallback
			}
			else
			{
				// Clamp frequency to valid range
				const f32 clampedFrequency = glm::clamp(frequency, minFrequency, maxFrequency);
				
				// Calculate octaves between minimum frequency and target frequency
				const f32 octavesBetweenMinAndTarget = glm::log2(clampedFrequency / minFrequency);
				
				// Calculate the total octave range
				const f32 octaveRange = glm::log2(maxFrequency / minFrequency);
				
				// Avoid division by zero
				if (octaveRange == 0.0f)
				{
					value = minValue;
				}
				else
				{
					// Calculate the output value range
					const f32 valueRange = maxValue - minValue;
					
					// Map from logarithmic frequency to linear value
					value = (octavesBetweenMinAndTarget / octaveRange) * valueRange + minValue;
				}
			}

			// Set output parameter
			SetParameterValue(Value_ID, value);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = value;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("FrequencyLogToLinearNode");
		}

		const char* GetDisplayName() const override
		{
			return "Frequency Log to Linear";
		}
	};

} // namespace OloEngine::Audio::SoundGraph