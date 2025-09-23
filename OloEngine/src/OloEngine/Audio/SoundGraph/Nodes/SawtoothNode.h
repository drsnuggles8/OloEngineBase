#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SawtoothNode - Generates sawtooth wave oscillation
	/// Based on Hazel's oscillator patterns
	class SawtoothNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier Phase_ID = OLO_IDENTIFIER("Phase");
		const Identifier Amplitude_ID = OLO_IDENTIFIER("Amplitude");
		const Identifier Direction_ID = OLO_IDENTIFIER("Direction"); // 1 = rising saw, -1 = falling saw
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Internal state
		f64 m_Phase = 0.0;
		f64 m_SampleRate = 44100.0;

	public:
		SawtoothNode()
		{
			// Register parameters
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);
			AddParameter<f32>(Phase_ID, "Phase", 0.0f);
			AddParameter<f32>(Amplitude_ID, "Amplitude", 1.0f);
			AddParameter<f32>(Direction_ID, "Direction", 1.0f); // Rising sawtooth by default
			AddParameter<f32>(Output_ID, "Output", 0.0f);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			m_Phase = 0.0;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			f32 frequency = GetParameterValue<f32>(Frequency_ID, 440.0f);
			f32 phaseOffset = GetParameterValue<f32>(Phase_ID, 0.0f);
			f32 amplitude = GetParameterValue<f32>(Amplitude_ID, 1.0f);
			f32 direction = GetParameterValue<f32>(Direction_ID, 1.0f);

			// Clamp frequency to reasonable range
			frequency = glm::clamp(frequency, 0.1f, static_cast<f32>(m_SampleRate * 0.5));

			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Calculate sawtooth wave: 2 * (phase - floor(phase + 0.5))
					f64 normalizedPhase = m_Phase + static_cast<f64>(phaseOffset) / (2.0 * glm::pi<f64>());
					normalizedPhase = normalizedPhase - glm::floor(normalizedPhase); // Keep in [0, 1)

					f32 sawtoothValue;
					if (direction >= 0.0f)
					{
						// Rising sawtooth: goes from -1 to 1
						sawtoothValue = static_cast<f32>(2.0 * normalizedPhase - 1.0);
					}
					else
					{
						// Falling sawtooth: goes from 1 to -1
						sawtoothValue = static_cast<f32>(1.0 - 2.0 * normalizedPhase);
					}

					outputs[0][i] = sawtoothValue * amplitude;

					// Update phase
					m_Phase += static_cast<f64>(frequency) / m_SampleRate;
					if (m_Phase >= 1.0)
						m_Phase -= 1.0;
				}

				// Update output parameter with last sample
				SetParameterValue(Output_ID, outputs[0][numSamples - 1]);
			}
			else
			{
				// If no output buffer, still update phase and calculate single value
				f64 normalizedPhase = m_Phase + static_cast<f64>(phaseOffset) / (2.0 * glm::pi<f64>());
				normalizedPhase = normalizedPhase - glm::floor(normalizedPhase);

				f32 sawtoothValue;
				if (direction >= 0.0f)
				{
					sawtoothValue = static_cast<f32>(2.0 * normalizedPhase - 1.0);
				}
				else
				{
					sawtoothValue = static_cast<f32>(1.0 - 2.0 * normalizedPhase);
				}

				f32 result = sawtoothValue * amplitude;
				SetParameterValue(Output_ID, result);

				// Update phase for one sample
				m_Phase += static_cast<f64>(frequency) / m_SampleRate;
				if (m_Phase >= 1.0)
					m_Phase -= 1.0;
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SawtoothNode");
		}

		const char* GetDisplayName() const override
		{
			return "Sawtooth Oscillator";
		}

		// Utility methods for external control
		f32 GetCurrentFrequency() const
		{
			return GetParameterValue<f32>(Frequency_ID, 440.0f);
		}

		f64 GetCurrentPhase() const
		{
			return m_Phase;
		}

		void ResetPhase(f64 phase = 0.0)
		{
			m_Phase = phase;
		}

		f32 GetDirection() const
		{
			return GetParameterValue<f32>(Direction_ID, 1.0f);
		}
	};
}