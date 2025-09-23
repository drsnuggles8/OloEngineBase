#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// AllPassFilterNode - Two-pole all-pass filter
	/// Passes all frequencies without amplitude change but alters phase relationships
	/// Essential for reverb algorithms, stereo widening, and phase manipulation effects
	class AllPassFilterNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier Resonance_ID = OLO_IDENTIFIER("Resonance");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Internal state
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

	public:
		AllPassFilterNode()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Frequency_ID, "Frequency", 1000.0f); // Characteristic frequency in Hz
			AddParameter<f32>(Resonance_ID, "Resonance", 1.0f);    // Q factor (0.1 to 10)
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
			f32 frequency = GetParameterValue<f32>(Frequency_ID, 1000.0f);
			f32 resonance = GetParameterValue<f32>(Resonance_ID, 1.0f);

			// Clamp frequency to reasonable range (avoid aliasing)
			frequency = glm::clamp(frequency, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
			// Clamp resonance to avoid instability
			resonance = glm::clamp(resonance, 0.1f, 10.0f);

			// Calculate biquad coefficients for all-pass filter
			f32 omega = 2.0f * glm::pi<f32>() * frequency / static_cast<f32>(m_SampleRate);
			f32 alpha = glm::sin(omega) / (2.0f * resonance);
			
			f32 cos_omega = glm::cos(omega);
			
			// All-pass filter coefficients (corrected)
			f32 b0 = 1.0f - alpha;
			f32 b1 = -2.0f * cos_omega;
			f32 b2 = 1.0f + alpha;
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
				
				// Calculate coefficients (same as above)
				f32 omega = 2.0f * glm::pi<f32>() * frequency / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * resonance);
				f32 cos_omega = glm::cos(omega);
				
				f32 b0 = 1.0f - alpha;
				f32 b1 = -2.0f * cos_omega;
				f32 b2 = 1.0f + alpha;
				f32 a0 = 1.0f + alpha;
				f32 a1 = -2.0f * cos_omega;
				f32 a2 = 1.0f - alpha;
				
				b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
				
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
			return OLO_IDENTIFIER("AllPassFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "All-Pass Filter";
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Get the current characteristic frequency (clamped to safe range)
		f32 GetFrequency() const
		{
			f32 frequency = GetParameterValue<f32>(Frequency_ID, 1000.0f);
			return glm::clamp(frequency, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
		}

		/// Get the current resonance factor
		f32 GetResonance() const
		{
			return glm::clamp(GetParameterValue<f32>(Resonance_ID, 1.0f), 0.1f, 10.0f);
		}

		/// Calculate the phase shift at a given frequency
		/// Returns phase shift in radians at the specified frequency
		f32 GetPhaseShiftAt(f32 testFreq) const
		{
			f32 charFreq = GetFrequency();
			f32 Q = GetResonance();
			
			// Simplified phase calculation for all-pass filter
			f32 omega = 2.0f * glm::pi<f32>() * testFreq / static_cast<f32>(m_SampleRate);
			f32 omega_c = 2.0f * glm::pi<f32>() * charFreq / static_cast<f32>(m_SampleRate);
			
			// Approximate phase shift calculation
			f32 ratio = testFreq / charFreq;
			if (ratio < 1.0f)
			{
				return -glm::atan(ratio * Q); // Phase lag below characteristic frequency
			}
			else
			{
				return -glm::pi<f32>() + glm::atan(Q / ratio); // Phase lead above characteristic frequency
			}
		}

		/// Reset the filter state to prevent audio artifacts
		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		/// Set characteristic frequency with validation
		void SetFrequency(f32 freq)
		{
			f32 clampedFreq = glm::clamp(freq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
			SetParameterValue(Frequency_ID, clampedFreq);
		}

		/// Set resonance with validation
		void SetResonance(f32 resonance)
		{
			f32 clampedResonance = glm::clamp(resonance, 0.1f, 10.0f);
			SetParameterValue(Resonance_ID, clampedResonance);
		}

		/// Check if the filter preserves amplitude (should always be true for all-pass)
		bool PreservesAmplitude() const
		{
			return true; // All-pass filters by definition preserve amplitude
		}

		/// Get the group delay at the characteristic frequency
		/// Group delay is the negative derivative of phase with respect to frequency
		f32 GetGroupDelay() const
		{
			f32 frequency = GetFrequency();
			f32 resonance = GetResonance();
			
			// Approximate group delay calculation for all-pass filter
			f32 omega = 2.0f * glm::pi<f32>() * frequency / static_cast<f32>(m_SampleRate);
			
			// Group delay is maximum at the characteristic frequency
			return resonance / (static_cast<f32>(m_SampleRate) * glm::sin(omega));
		}
	};

} // namespace OloEngine::Audio::SoundGraph