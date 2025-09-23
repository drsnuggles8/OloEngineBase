#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// CosineNode - A cosine wave oscillator for audio synthesis
	/// Generates clean cosine waves with controllable frequency and phase
	/// Provides 90-degree phase shift from sine wave, useful for quadrature oscillators
	/// and stereo effects
	class CosineNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier PhaseOffset_ID = OLO_IDENTIFIER("PhaseOffset");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Oscillator state
		f64 m_Phase = 0.0;
		f64 m_PhaseIncrement = 0.0;
		f64 m_SampleRate = 48000.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		CosineNode()
		{
			// Register parameters directly
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);     // Default to A4 (440 Hz)
			AddParameter<f32>(PhaseOffset_ID, "PhaseOffset", 0.0f);   // Phase offset in radians
			AddParameter<f32>(Output_ID, "Output", 0.0f);             // Cosine wave output
		}

		virtual ~CosineNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 frequency = glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
			const f32 phaseOffset = GetParameterValue<f32>(PhaseOffset_ID);
			
			// Calculate phase increment for this frequency
			m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
			
			// Generate cosine wave samples
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Add phase offset and generate cosine wave
					const f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
					const f32 cosineValue = static_cast<f32>(glm::cos(currentPhase));
					
					outputs[0][i] = cosineValue;
					
					// Advance phase and wrap around 2π
					m_Phase += m_PhaseIncrement;
					if (m_Phase >= glm::two_pi<f64>())
					{
						m_Phase -= glm::two_pi<f64>();
					}
				}
				
				// Set output parameter to the last generated value
				SetParameterValue(Output_ID, outputs[0][numSamples - 1]);
			}
			else
			{
				// Generate single value if no output buffer
				const f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
				const f32 cosineValue = static_cast<f32>(glm::cos(currentPhase));
				
				SetParameterValue(Output_ID, cosineValue);
				
				// Advance phase for next call
				m_Phase += m_PhaseIncrement * numSamples;
				if (m_Phase >= glm::two_pi<f64>())
				{
					m_Phase = fmod(m_Phase, glm::two_pi<f64>());
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize phase and calculate initial phase increment
			m_Phase = static_cast<f64>(GetParameterValue<f32>(PhaseOffset_ID));
			const f32 frequency = glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
			m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("CosineNode");
		}

		const char* GetDisplayName() const override
		{
			return "Cosine Oscillator";
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Reset the oscillator phase to the specified offset
		void ResetPhase()
		{
			m_Phase = static_cast<f64>(GetParameterValue<f32>(PhaseOffset_ID));
		}

		/// Reset the oscillator phase to a specific value
		void ResetPhase(f32 phase)
		{
			m_Phase = static_cast<f64>(phase);
		}

		/// Get the current phase (for visualization or debugging)
		f64 GetCurrentPhase() const
		{
			return m_Phase;
		}

		/// Get the current frequency (clamped to safe range)
		f32 GetCurrentFrequency() const
		{
			return glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
		}
	};

} // namespace OloEngine::Audio::SoundGraph