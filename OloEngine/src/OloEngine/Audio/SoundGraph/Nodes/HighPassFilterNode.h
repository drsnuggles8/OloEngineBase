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
	/// HighPassFilterNode - Simple high-pass filter using new Hazel-style foundation
	/// Implements a biquad high-pass filter with controllable cutoff and resonance
	class HighPassFilterNode : public NodeProcessor
	{
	private:
		// Input streams for connected values
		ValueView<f32> m_InputStream;
		ValueView<f32> m_CutoffStream;
		ValueView<f32> m_ResonanceStream;
		
		// Output stream for filtered audio
		ValueView<f32> m_OutputStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentCutoff = 1000.0f;  // Hz
		f32 m_CurrentResonance = 0.7f;  // Q factor
		f32 m_CurrentOutput = 0.0f;

		// Internal filter state
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

		// Filter parameter limits
		static constexpr f32 MIN_CUTOFF_HZ = 20.0f;
		static constexpr f32 MIN_RESONANCE = 0.1f;
		static constexpr f32 MAX_RESONANCE = 10.0f;

	public:
		HighPassFilterNode()
		{
			// Create input events for receiving parameter values
			auto inputEvent = std::make_shared<InputEvent>("Input", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentInput = value.Get<f32>();
				}
			});
			
			auto cutoffEvent = std::make_shared<InputEvent>("Cutoff", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					f32 maxCutoff = static_cast<f32>(m_SampleRate * 0.45);  // Avoid aliasing
					m_CurrentCutoff = glm::clamp(value.Get<f32>(), MIN_CUTOFF_HZ, maxCutoff);
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
			AddInputEvent(cutoffEvent);
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
			m_CutoffStream = ValueView<f32>(maxBufferSize);  
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
				f32 cutoff = m_CurrentCutoff;
				f32 resonance = m_CurrentResonance;

				// Clamp parameters to safe ranges
				f32 maxCutoff = static_cast<f32>(m_SampleRate * 0.45);
				cutoff = glm::clamp(cutoff, MIN_CUTOFF_HZ, maxCutoff);
				resonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);

				// Calculate biquad filter coefficients for high-pass
				f32 omega = 2.0f * glm::pi<f32>() * cutoff / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * resonance);
				
				f32 cos_omega = glm::cos(omega);
				f32 b0 = (1.0f + cos_omega) / 2.0f;      // High-pass coefficients
				f32 b1 = -(1.0f + cos_omega);           // Different from low-pass
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
				f32 cutoff = m_CurrentCutoff;
				f32 resonance = m_CurrentResonance;

				// Clamp parameters to safe ranges
				f32 maxCutoff = static_cast<f32>(m_SampleRate * 0.45);
				cutoff = glm::clamp(cutoff, MIN_CUTOFF_HZ, maxCutoff);
				resonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);

				// Calculate biquad filter coefficients for high-pass
				f32 omega = 2.0f * glm::pi<f32>() * cutoff / static_cast<f32>(m_SampleRate);
				f32 alpha = glm::sin(omega) / (2.0f * resonance);
				
				f32 cos_omega = glm::cos(omega);
				f32 b0 = (1.0f + cos_omega) / 2.0f;      // High-pass coefficients
				f32 b1 = -(1.0f + cos_omega);           // Different from low-pass
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
			return OLO_IDENTIFIER("HighPassFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "High-Pass Filter";
		}

		// Legacy compatibility methods for direct access
		f32 GetCutoffFrequency() const { return m_CurrentCutoff; }
		f32 GetResonance() const { return m_CurrentResonance; }
		f32 GetOutput() const { return m_CurrentOutput; }
		
		void SetInput(f32 value) { m_CurrentInput = value; }
		void SetCutoffFrequency(f32 cutoff) 
		{ 
			f32 maxCutoff = static_cast<f32>(m_SampleRate * 0.45);
			m_CurrentCutoff = glm::clamp(cutoff, MIN_CUTOFF_HZ, maxCutoff);
		}
		void SetResonance(f32 resonance) 
		{ 
			m_CurrentResonance = glm::clamp(resonance, MIN_RESONANCE, MAX_RESONANCE);
		}

		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		// ValueView accessors for advanced stream processing
		const ValueView<f32>& GetInputStream() const { return m_InputStream; }
		const ValueView<f32>& GetCutoffStream() const { return m_CutoffStream; }
		const ValueView<f32>& GetResonanceStream() const { return m_ResonanceStream; }
		const ValueView<f32>& GetOutputStream() const { return m_OutputStream; }
	};
}