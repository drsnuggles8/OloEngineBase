#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// HighPassFilterNode - Simple one-pole high-pass filter
	/// Implements a basic RC-style high-pass filter
	class HighPassFilterNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Cutoff_ID = OLO_IDENTIFIER("Cutoff");
		const Identifier Resonance_ID = OLO_IDENTIFIER("Resonance");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Internal state
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

	public:
		HighPassFilterNode()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Cutoff_ID, "Cutoff", 1000.0f); // Cutoff frequency in Hz
			AddParameter<f32>(Resonance_ID, "Resonance", 0.7f); // Q factor (0.1 to 10)
			AddParameter<f32>(Output_ID, "Output", 0.0f);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			f32 cutoff = GetParameterValue<f32>(Cutoff_ID, 1000.0f);
			f32 resonance = GetParameterValue<f32>(Resonance_ID, 0.7f);

			// Clamp cutoff to reasonable range
			cutoff = glm::clamp(cutoff, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
			// Clamp resonance to avoid instability
			resonance = glm::clamp(resonance, 0.1f, 10.0f);

			// Calculate filter coefficients for high-pass biquad
			f32 omega = 2.0f * glm::pi<f32>() * cutoff / static_cast<f32>(m_SampleRate);
			f32 alpha = glm::sin(omega) / (2.0f * resonance);
			
			f32 cos_omega = glm::cos(omega);
			f32 b0 = (1.0f + cos_omega) / 2.0f;
			f32 b1 = -(1.0f + cos_omega);
			f32 b2 = (1.0f + cos_omega) / 2.0f;
			f32 a0 = 1.0f + alpha;
			f32 a1 = -2.0f * cos_omega;
			f32 a2 = 1.0f - alpha;

			// Normalize coefficients
			b0 /= a0;
			b1 /= a0;
			b2 /= a0;
			a1 /= a0;
			a2 /= a0;

			if (inputs && inputs[0] && outputs && outputs[0])
			{
				// Process input buffer
				for (u32 i = 0; i < numSamples; ++i)
				{
					f32 inputSample = inputs[0][i];
					
					// Biquad filter implementation
					f32 output = b0 * inputSample + b1 * m_PreviousInput + b2 * m_PreviousInput2
							   - a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
					
					outputs[0][i] = output;
					
					// Update state
					m_PreviousInput2 = m_PreviousInput;
					m_PreviousInput = inputSample;
					m_PreviousOutput2 = m_PreviousOutput;
					m_PreviousOutput = output;
				}

				// Update output parameter with last sample
				SetParameterValue(Output_ID, outputs[0][numSamples - 1]);
			}
			else
			{
				// Process single input parameter
				f32 inputSample = GetParameterValue<f32>(Input_ID, 0.0f);
				
				f32 output = b0 * inputSample + b1 * m_PreviousInput + b2 * m_PreviousInput2
						   - a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
				
				SetParameterValue(Output_ID, output);
				
				// Update state
				m_PreviousInput2 = m_PreviousInput;
				m_PreviousInput = inputSample;
				m_PreviousOutput2 = m_PreviousOutput;
				m_PreviousOutput = output;
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("HighPassFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "High-Pass Filter";
		}

		// Utility methods
		f32 GetCutoffFrequency() const
		{
			return GetParameterValue<f32>(Cutoff_ID, 1000.0f);
		}

		f32 GetResonance() const
		{
			return GetParameterValue<f32>(Resonance_ID, 0.7f);
		}

		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousInput = 0.0f;
		}
	};
}