#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// BandPassFilterNode - Band-pass filter using new Hazel-style foundation
	/// Allows frequencies within a specific range to pass through while attenuating others
	/// Ideal for isolating frequency bands and creating frequency-selective effects
	class BandPassFilterNode : public NodeProcessor
	{
	private:
		// Input streams for connected values
		ValueView<f32> m_InputStream;
		ValueView<f32> m_CenterFreqStream;
		ValueView<f32> m_BandwidthStream;
		ValueView<f32> m_ResonanceStream;
		
		// Output stream for filtered audio
		ValueView<f32> m_OutputStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentCenterFreq = 1000.0f;  // Hz
		f32 m_CurrentBandwidth = 200.0f;    // Hz
		f32 m_CurrentResonance = 1.0f;      // Q factor
		f32 m_CurrentOutput = 0.0f;

		// Internal filter state
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

		// Filter parameter limits
		static constexpr f32 MIN_CENTER_FREQ_HZ = 20.0f;
		static constexpr f32 MIN_BANDWIDTH_HZ = 1.0f;
		static constexpr f32 MIN_RESONANCE = 0.1f;
		static constexpr f32 MAX_RESONANCE = 10.0f;

	public:
		BandPassFilterNode()
		{
			// Create input events for receiving parameter values
			auto inputEvent = std::make_shared<InputEvent>("Input", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentInput = value.Get<f32>();
				}
			});
			
			auto centerFreqEvent = std::make_shared<InputEvent>("CenterFreq", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					f32 maxFreq = static_cast<f32>(m_SampleRate * 0.45);  // Avoid aliasing
					m_CurrentCenterFreq = glm::clamp(value.Get<f32>(), MIN_CENTER_FREQ_HZ, maxFreq);
				}
			});

			auto bandwidthEvent = std::make_shared<InputEvent>("Bandwidth", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					// Clamp bandwidth to prevent degenerate cases
					f32 maxBandwidth = m_CurrentCenterFreq;
					m_CurrentBandwidth = glm::clamp(value.Get<f32>(), MIN_BANDWIDTH_HZ, maxBandwidth);
				}
			});

			auto resonanceEvent = std::make_shared<InputEvent>("Resonance", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentResonance = glm::clamp(value.Get<f32>(), MIN_RESONANCE, MAX_RESONANCE);
				}
			});

			// Register input events
			AddInputEvent(inputEvent);
			AddInputEvent(centerFreqEvent);
			AddInputEvent(bandwidthEvent);
			AddInputEvent(resonanceEvent);

			// Create output event for sending filtered audio
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			AddOutputEvent(outputEvent);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize ValueView streams with proper capacity
			m_InputStream = ValueView<f32>(maxBufferSize);
			m_CenterFreqStream = ValueView<f32>(maxBufferSize);  
			m_BandwidthStream = ValueView<f32>(maxBufferSize);
			m_ResonanceStream = ValueView<f32>(maxBufferSize);
			m_OutputStream = ValueView<f32>(maxBufferSize);

			// Reset filter state
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Handle stream processing mode (real-time audio)
			if (inputs && inputs[0] && outputs && outputs[0])
			{
				// Fill input stream from audio buffer
				for (u32 i = 0; i < numSamples; ++i)
				{
					m_InputStream[i] = inputs[0][i];
				}

				// Get current parameter values for coefficient calculation
				f32 centerFreq = m_CurrentCenterFreq;
				f32 bandwidth = m_CurrentBandwidth;
				f32 resonance = m_CurrentResonance;

				// Clamp parameters to safe ranges
				f32 maxFreq = static_cast<f32>(m_SampleRate * 0.45);
				centerFreq = glm::clamp(centerFreq, MIN_CENTER_FREQ_HZ, maxFreq);
				bandwidth = glm::clamp(bandwidth, MIN_BANDWIDTH_HZ, centerFreq);
				resonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);

				// Calculate Q from bandwidth and apply resonance scaling
				f32 Q = (centerFreq / bandwidth) * resonance;
				Q = glm::clamp(Q, 0.1f, 30.0f); // Prevent extreme Q values

				// Calculate biquad coefficients for band-pass filter
				f32 omega = 2.0f * glm::pi<f32>() * centerFreq / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * Q);
				
				f32 cos_omega = glm::cos(omega);
				
				// Band-pass filter coefficients
				f32 b0 = alpha;
				f32 b1 = 0.0f;
				f32 b2 = -alpha;
				f32 a0 = 1.0f + alpha;
				f32 a1 = -2.0f * cos_omega;
				f32 a2 = 1.0f - alpha;

				// Normalize coefficients
				b0 /= a0;
				b1 /= a0;
				b2 /= a0;
				a1 /= a0;
				a2 /= a0;

				// Process samples and fill output stream
				for (u32 i = 0; i < numSamples; ++i)
				{
					f32 inputSample = m_InputStream[i];
					
					// Biquad filter implementation
					f32 output = b0 * inputSample + b1 * m_PreviousInput + b2 * m_PreviousInput2
							   - a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
					
					m_OutputStream[i] = output;
					outputs[0][i] = output;
					
					// Update filter state
					m_PreviousInput2 = m_PreviousInput;
					m_PreviousInput = inputSample;
					m_PreviousOutput2 = m_PreviousOutput;
					m_PreviousOutput = output;
				}

				// Update current output value with last sample
				m_CurrentOutput = m_OutputStream[numSamples - 1];

				// Send output via event system
				auto outputEvent = GetOutputEvent("Output");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
			}
			else
			{
				// Handle single-value processing mode (control parameters)
				f32 inputSample = m_CurrentInput;
				f32 centerFreq = m_CurrentCenterFreq;
				f32 bandwidth = m_CurrentBandwidth;
				f32 resonance = m_CurrentResonance;

				// Clamp parameters to safe ranges
				f32 maxFreq = static_cast<f32>(m_SampleRate * 0.45);
				centerFreq = glm::clamp(centerFreq, MIN_CENTER_FREQ_HZ, maxFreq);
				bandwidth = glm::clamp(bandwidth, MIN_BANDWIDTH_HZ, centerFreq);
				resonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);

				// Calculate Q from bandwidth and apply resonance scaling
				f32 Q = (centerFreq / bandwidth) * resonance;
				Q = glm::clamp(Q, 0.1f, 30.0f);

				// Calculate biquad coefficients for band-pass filter
				f32 omega = 2.0f * glm::pi<f32>() * centerFreq / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * Q);
				
				f32 cos_omega = glm::cos(omega);
				
				// Band-pass filter coefficients
				f32 b0 = alpha;
				f32 b1 = 0.0f;
				f32 b2 = -alpha;
				f32 a0 = 1.0f + alpha;
				f32 a1 = -2.0f * cos_omega;
				f32 a2 = 1.0f - alpha;

				// Normalize coefficients
				b0 /= a0;
				b1 /= a0;
				b2 /= a0;
				a1 /= a0;
				a2 /= a0;

				// Process single sample
				f32 output = b0 * inputSample + b1 * m_PreviousInput + b2 * m_PreviousInput2
						   - a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
				
				m_CurrentOutput = output;
				
				// Update filter state
				m_PreviousInput2 = m_PreviousInput;
				m_PreviousInput = inputSample;
				m_PreviousOutput2 = m_PreviousOutput;
				m_PreviousOutput = output;

				// Send output via event system
				auto outputEvent = GetOutputEvent("Output");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("BandPassFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "Band-Pass Filter";
		}

		//======================================================================
		// Legacy Compatibility & Utility Methods
		//======================================================================

		// Legacy compatibility methods for direct access
		f32 GetCenterFrequency() const { return m_CurrentCenterFreq; }
		f32 GetBandwidth() const { return m_CurrentBandwidth; }
		f32 GetResonance() const { return m_CurrentResonance; }
		f32 GetOutput() const { return m_CurrentOutput; }
		
		void SetInput(f32 value) { m_CurrentInput = value; }
		void SetCenterFrequency(f32 freq) 
		{ 
			f32 maxFreq = static_cast<f32>(m_SampleRate * 0.45);
			m_CurrentCenterFreq = glm::clamp(freq, MIN_CENTER_FREQ_HZ, maxFreq);
		}
		void SetBandwidth(f32 bandwidth) 
		{ 
			m_CurrentBandwidth = glm::clamp(bandwidth, MIN_BANDWIDTH_HZ, m_CurrentCenterFreq);
		}
		void SetResonance(f32 resonance) 
		{ 
			m_CurrentResonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);
		}

		/// Calculate the effective Q factor from current parameters
		f32 GetEffectiveQ() const
		{
			f32 centerFreq = m_CurrentCenterFreq;
			f32 bandwidth = m_CurrentBandwidth;
			f32 resonance = m_CurrentResonance;
			
			f32 Q = (centerFreq / bandwidth) * resonance;
			return glm::clamp(Q, 0.1f, 30.0f);
		}

		/// Get the approximate low cutoff frequency (-3dB point)
		f32 GetLowCutoff() const
		{
			return glm::max(MIN_CENTER_FREQ_HZ, m_CurrentCenterFreq - m_CurrentBandwidth * 0.5f);
		}

		/// Get the approximate high cutoff frequency (-3dB point)
		f32 GetHighCutoff() const
		{
			f32 maxFreq = static_cast<f32>(m_SampleRate * 0.45);
			return glm::min(maxFreq, m_CurrentCenterFreq + m_CurrentBandwidth * 0.5f);
		}

		/// Reset the filter state to prevent audio artifacts
		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		// ValueView accessors for advanced stream processing
		const ValueView<f32>& GetInputStream() const { return m_InputStream; }
		const ValueView<f32>& GetCenterFreqStream() const { return m_CenterFreqStream; }
		const ValueView<f32>& GetBandwidthStream() const { return m_BandwidthStream; }
		const ValueView<f32>& GetResonanceStream() const { return m_ResonanceStream; }
		const ValueView<f32>& GetOutputStream() const { return m_OutputStream; }
	};

} // namespace OloEngine::Audio::SoundGraph