#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SineNode - A sine wave oscillator for audio synthesis
	/// Generates clean sine waves with controllable frequency and phase
	/// Essential building block for audio synthesis and signal generation
	class SineNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier PhaseOffset_ID = OLO_IDENTIFIER("PhaseOffset");
		const Identifier ResetPhase_ID = OLO_IDENTIFIER("ResetPhase");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Oscillator state
		f64 m_Phase = 0.0;
		f64 m_PhaseIncrement = 0.0;
		f64 m_SampleRate = 48000.0;

		// Event flag for ResetPhase
		Flag m_ResetPhaseFlag;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		SineNode()
		{
			// Register parameters directly
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);     // Default to A4 (440 Hz)
			AddParameter<f32>(PhaseOffset_ID, "PhaseOffset", 0.0f);   // Phase offset in radians
			AddParameter<f32>(ResetPhase_ID, "ResetPhase", 0.0f);     // ResetPhase trigger
			AddParameter<f32>(Output_ID, "Output", 0.0f);             // Sine wave output

			// Register ResetPhase input event with flag callback
			AddInputEvent<f32>(ResetPhase_ID, "ResetPhase", [this](f32 value) {
				if (value > 0.5f) m_ResetPhaseFlag.SetDirty();
			});
		}

		virtual ~SineNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for ResetPhase trigger via parameter or flag
			f32 resetPhaseValue = GetParameterValue<f32>(ResetPhase_ID);
			
			if (resetPhaseValue > 0.5f || m_ResetPhaseFlag.CheckAndResetIfDirty())
			{
				ResetPhase();
				// Reset trigger parameter
				if (resetPhaseValue > 0.5f)
					SetParameterValue(ResetPhase_ID, 0.0f);
			}

			const f32 frequency = glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
			const f32 phaseOffset = GetParameterValue<f32>(PhaseOffset_ID);
			
			// Calculate phase increment for this frequency
			m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
			
			// Generate sine wave samples
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Add phase offset and generate sine wave
					const f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
					const f32 sineValue = static_cast<f32>(glm::sin(currentPhase));
					
					outputs[0][i] = sineValue;
					
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
				const f32 sineValue = static_cast<f32>(glm::sin(currentPhase));
				
				SetParameterValue(Output_ID, sineValue);
				
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
			return OLO_IDENTIFIER("SineNode");
		}

		const char* GetDisplayName() const override
		{
			return "Sine Oscillator";
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