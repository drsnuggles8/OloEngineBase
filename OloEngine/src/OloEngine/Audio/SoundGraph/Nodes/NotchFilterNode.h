#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// NotchFilterNode - Two-pole notch filter (band-stop filter)
	/// Attenuates frequencies within a specific range while allowing others to pass
	/// Ideal for removing specific frequency bands, feedback elimination, and tone shaping
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class NotchFilterNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<f32> m_InputView;
		InputView<f32> m_CenterFreqView;
		InputView<f32> m_BandwidthView;
		InputView<f32> m_ResonanceView;
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentCenterFreq = 1000.0f;
		f32 m_CurrentBandwidth = 200.0f;
		f32 m_CurrentResonance = 1.0f;
		f32 m_CurrentOutput = 0.0f;

		//======================================================================
		// Filter State
		//======================================================================
		
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

	public:
		NotchFilterNode()
			: m_InputView([this](f32 value) { m_CurrentInput = value; }),
			  m_CenterFreqView([this](f32 value) { m_CurrentCenterFreq = value; }),
			  m_BandwidthView([this](f32 value) { m_CurrentBandwidth = value; }),
			  m_ResonanceView([this](f32 value) { m_CurrentResonance = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_CenterFreqView.Initialize(maxBufferSize);
			m_BandwidthView.Initialize(maxBufferSize);
			m_ResonanceView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize filter state
			m_SampleRate = sampleRate;
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_CenterFreqView.UpdateFromConnections(inputs, numSamples);
			m_BandwidthView.UpdateFromConnections(inputs, numSamples);
			m_ResonanceView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 inputValue = m_InputView.GetValue(sample);
				f32 centerFreq = m_CenterFreqView.GetValue(sample);
				f32 bandwidth = m_BandwidthView.GetValue(sample);
				f32 resonance = m_ResonanceView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				m_CurrentCenterFreq = centerFreq;
				m_CurrentBandwidth = bandwidth;
				m_CurrentResonance = resonance;

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
				
				// Apply biquad filter: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
				f32 output = b0 * inputValue + b1 * m_PreviousInput + b2 * m_PreviousInput2
							- a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
				
				// Update delay line
				m_PreviousInput2 = m_PreviousInput;
				m_PreviousInput = inputValue;
				m_PreviousOutput2 = m_PreviousOutput;
				m_PreviousOutput = output;
				
				// Set output value
				m_CurrentOutput = output;
				m_OutputView.SetValue(sample, output);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
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
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("CenterFreq")) m_CurrentCenterFreq = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Bandwidth")) m_CurrentBandwidth = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Resonance")) m_CurrentResonance = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("CenterFreq")) return static_cast<T>(m_CurrentCenterFreq);
			else if (id == OLO_IDENTIFIER("Bandwidth")) return static_cast<T>(m_CurrentBandwidth);
			else if (id == OLO_IDENTIFIER("Resonance")) return static_cast<T>(m_CurrentResonance);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentOutput);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Reset the internal filter state (useful for clearing transients)
		void ResetFilterState()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		/// Get the current center frequency (clamped to safe range)
		f32 GetCenterFrequency() const
		{
			return glm::clamp(m_CurrentCenterFreq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
		}

		/// Get the current bandwidth (clamped to safe range)
		f32 GetBandwidth() const
		{
			f32 centerFreq = GetCenterFrequency();
			return glm::clamp(m_CurrentBandwidth, 1.0f, centerFreq);
		}

		/// Get the current resonance factor
		f32 GetResonance() const
		{
			return glm::clamp(m_CurrentResonance, 0.1f, 10.0f);
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

		/// Set center frequency with validation
		void SetCenterFrequency(f32 freq)
		{
			m_CurrentCenterFreq = glm::clamp(freq, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
		}

		/// Set bandwidth with validation
		void SetBandwidth(f32 bandwidth)
		{
			f32 centerFreq = GetCenterFrequency();
			m_CurrentBandwidth = glm::clamp(bandwidth, 1.0f, centerFreq);
		}
	};

} // namespace OloEngine::Audio::SoundGraph