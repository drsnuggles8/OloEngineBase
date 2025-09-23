#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// PulseNode - A pulse wave oscillator with variable duty cycle (PWM)
	/// Generates rectangular pulse waves with controllable pulse width
	/// Essential for classic synthesizer sounds and pulse width modulation effects
	class PulseNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier PulseWidth_ID = OLO_IDENTIFIER("PulseWidth");
		const Identifier PhaseOffset_ID = OLO_IDENTIFIER("PhaseOffset");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Oscillator state
		f64 m_Phase = 0.0;
		f64 m_PhaseIncrement = 0.0;
		f64 m_SampleRate = 48000.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

		// Pulse width limits (0.0 = 0%, 1.0 = 100%)
		static constexpr f32 MIN_PULSE_WIDTH = 0.001f;  // Prevent completely silent output
		static constexpr f32 MAX_PULSE_WIDTH = 0.999f;  // Prevent DC offset

	public:
		PulseNode()
		{
			// Register parameters directly
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);       // Default to A4 (440 Hz)
			AddParameter<f32>(PulseWidth_ID, "PulseWidth", 0.5f);       // Default to 50% duty cycle (square wave)
			AddParameter<f32>(PhaseOffset_ID, "PhaseOffset", 0.0f);     // Phase offset in radians
			AddParameter<f32>(Output_ID, "Output", 0.0f);               // Pulse wave output
		}

		virtual ~PulseNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 frequency = glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
			const f32 pulseWidth = glm::clamp(GetParameterValue<f32>(PulseWidth_ID), MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
			const f32 phaseOffset = GetParameterValue<f32>(PhaseOffset_ID);
			
			// Calculate phase increment for this frequency
			m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
			
			// Generate pulse wave samples
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Add phase offset and normalize to [0, 1] range
					f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
					f64 normalizedPhase = fmod(currentPhase, glm::two_pi<f64>()) / glm::two_pi<f64>();
					if (normalizedPhase < 0.0) normalizedPhase += 1.0;
					
					// Generate pulse wave: +1.0 when phase < pulseWidth, -1.0 otherwise
					const f64 pulseWidthAsDouble = static_cast<f64>(pulseWidth);
					const f32 pulseValue = (normalizedPhase < pulseWidthAsDouble) ? 1.0f : -1.0f;
					
					outputs[0][i] = pulseValue;
					
					// Advance phase and wrap around 2π
					m_Phase += m_PhaseIncrement;
					while (m_Phase >= glm::two_pi<f64>())
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
				f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
				f64 normalizedPhase = fmod(currentPhase, glm::two_pi<f64>()) / glm::two_pi<f64>();
				if (normalizedPhase < 0.0) normalizedPhase += 1.0;
				
				const f32 pulseValue = (normalizedPhase < static_cast<f64>(pulseWidth)) ? 1.0f : -1.0f;
				
				SetParameterValue(Output_ID, pulseValue);
				
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
			
			// Initialize phase to 0.0 (ignore phase offset for initial state)
			m_Phase = 0.0;
			const f32 frequency = glm::clamp(GetParameterValue<f32>(Frequency_ID), MIN_FREQ_HZ, MAX_FREQ_HZ);
			m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("PulseNode");
		}

		const char* GetDisplayName() const override
		{
			return "Pulse/PWM Oscillator";
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

		/// Get the current pulse width (clamped to safe range)
		f32 GetCurrentPulseWidth() const
		{
			return glm::clamp(GetParameterValue<f32>(PulseWidth_ID), MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
		}

		/// Set pulse width with validation
		void SetPulseWidth(f32 width)
		{
			const f32 clampedWidth = glm::clamp(width, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
			SetParameterValue(PulseWidth_ID, clampedWidth);
		}

		/// Get valid pulse width range
		static std::pair<f32, f32> GetPulseWidthRange()
		{
			return { MIN_PULSE_WIDTH, MAX_PULSE_WIDTH };
		}
	};

} // namespace OloEngine::Audio::SoundGraph