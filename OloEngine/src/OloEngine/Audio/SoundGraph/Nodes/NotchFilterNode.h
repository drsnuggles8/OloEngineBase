#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// NotchFilterNode - Two-pole notch filter (band-stop filter)
	/// Attenuates frequencies within a specific range while allowing others to pass
	/// Ideal for removing specific frequency bands, feedback elimination, and tone shaping
	class NotchFilterNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier CenterFreq_ID = OLO_IDENTIFIER("CenterFreq");
		const Identifier Bandwidth_ID = OLO_IDENTIFIER("Bandwidth");
		const Identifier Resonance_ID = OLO_IDENTIFIER("Resonance");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Internal state
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

	public:
		NotchFilterNode()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(CenterFreq_ID, "CenterFreq", 1000.0f); // Center frequency in Hz
			AddParameter<f32>(Bandwidth_ID, "Bandwidth", 200.0f);    // Bandwidth in Hz
			AddParameter<f32>(Resonance_ID, "Resonance", 1.0f);      // Q factor (0.1 to 10)
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
			f32 centerFreq = GetParameterValue<f32>(CenterFreq_ID, 1000.0f);
			f32 bandwidth = GetParameterValue<f32>(Bandwidth_ID, 200.0f);
			f32 resonance = GetParameterValue<f32>(Resonance_ID, 1.0f);

			// Clamp center frequency to reasonable range (avoid aliasing)
			centerFreq = glm::clamp(centerFreq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
			// Clamp bandwidth to prevent degenerate cases
			bandwidth = glm::clamp(bandwidth, 1.0f, centerFreq);
			// Clamp resonance to avoid instability
			resonance = glm::clamp(resonance, 0.1f, 10.0f);

			// Calculate Q from bandwidth: Q = center_freq / bandwidth
			f32 Q = centerFreq / bandwidth;
			// Apply user resonance scaling
			Q *= resonance;
			Q = glm::clamp(Q, 0.1f, 30.0f); // Prevent extreme Q values

			// Calculate biquad coefficients for notch filter
			f32 omega = 2.0f * glm::pi<f32>() * centerFreq / static_cast<f32>(m_SampleRate);
			f32 alpha = glm::sin(omega) / (2.0f * Q);
			
			f32 cos_omega = glm::cos(omega);
			
			// Notch filter coefficients (band-stop)
			f32 b0 = 1.0f;
			f32 b1 = -2.0f * cos_omega;
			f32 b2 = 1.0f;
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
				f32 Q = (centerFreq / bandwidth) * resonance;
				Q = glm::clamp(Q, 0.1f, 30.0f);
				
				f32 omega = 2.0f * glm::pi<f32>() * centerFreq / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * Q);
				f32 cos_omega = glm::cos(omega);
				
				f32 b0 = 1.0f;
				f32 b1 = -2.0f * cos_omega;
				f32 b2 = 1.0f;
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
			return OLO_IDENTIFIER("NotchFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "Notch Filter";
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Get the current center frequency (clamped to safe range)
		f32 GetCenterFrequency() const
		{
			f32 centerFreq = GetParameterValue<f32>(CenterFreq_ID, 1000.0f);
			return glm::clamp(centerFreq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
		}

		/// Get the current bandwidth (clamped to safe range)
		f32 GetBandwidth() const
		{
			f32 bandwidth = GetParameterValue<f32>(Bandwidth_ID, 200.0f);
			f32 centerFreq = GetCenterFrequency();
			return glm::clamp(bandwidth, 1.0f, centerFreq);
		}

		/// Get the current resonance factor
		f32 GetResonance() const
		{
			return glm::clamp(GetParameterValue<f32>(Resonance_ID, 1.0f), 0.1f, 10.0f);
		}

		/// Calculate the effective Q factor from current parameters
		f32 GetEffectiveQ() const
		{
			f32 centerFreq = GetCenterFrequency();
			f32 bandwidth = GetBandwidth();
			f32 resonance = GetResonance();
			
			f32 Q = (centerFreq / bandwidth) * resonance;
			return glm::clamp(Q, 0.1f, 30.0f);
		}

		/// Get the approximate low cutoff frequency (start of notch)
		f32 GetLowCutoff() const
		{
			f32 centerFreq = GetCenterFrequency();
			f32 bandwidth = GetBandwidth();
			return glm::max(20.0f, centerFreq - bandwidth * 0.5f);
		}

		/// Get the approximate high cutoff frequency (end of notch)
		f32 GetHighCutoff() const
		{
			f32 centerFreq = GetCenterFrequency();
			f32 bandwidth = GetBandwidth();
			return glm::min(static_cast<f32>(m_SampleRate * 0.45), centerFreq + bandwidth * 0.5f);
		}

		/// Reset the filter state to prevent audio artifacts
		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		/// Set center frequency with validation
		void SetCenterFrequency(f32 freq)
		{
			f32 clampedFreq = glm::clamp(freq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
			SetParameterValue(CenterFreq_ID, clampedFreq);
		}

		/// Set bandwidth with validation
		void SetBandwidth(f32 bandwidth)
		{
			f32 centerFreq = GetCenterFrequency();
			f32 clampedBandwidth = glm::clamp(bandwidth, 1.0f, centerFreq);
			SetParameterValue(Bandwidth_ID, clampedBandwidth);
		}
	};

} // namespace OloEngine::Audio::SoundGraph