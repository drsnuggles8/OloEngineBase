#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SquareNode - Generates square wave oscillation with duty cycle control
	/// Supports controllable frequency, phase, amplitude, and duty cycle parameters
	class SquareNode : public NodeProcessor
	{
	private:
		// Parameter streams
		InputView<f32> m_FrequencyInput;
		InputView<f32> m_PhaseInput;
		InputView<f32> m_AmplitudeInput;
		InputView<f32> m_DutyCycleInput;
		
		// Output stream for generated square wave
		OutputView<f32> m_Output;

		// Internal oscillator state
		f64 m_Phase = 0.0;
		f64 m_SampleRate = 44100.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		SquareNode()
		{
			// Initialize input streams with default values
			m_FrequencyInput = CreateInputView<f32>("Frequency", 440.0f);
			m_PhaseInput = CreateInputView<f32>("Phase", 0.0f);
			m_AmplitudeInput = CreateInputView<f32>("Amplitude", 1.0f);
			m_DutyCycleInput = CreateInputView<f32>("DutyCycle", 0.5f);
			
			// Initialize output stream
			m_Output = CreateOutputView<f32>("Output");
			
			// Register input event callbacks for real-time parameter updates
			m_FrequencyInput.RegisterInputEvent([this](f32 value) {
				// Clamp frequency to safe audio range
			});
			
			m_PhaseInput.RegisterInputEvent([this](f32 value) {
				// Phase offset handling
			});
			
			m_AmplitudeInput.RegisterInputEvent([this](f32 value) {
				// Amplitude scaling
			});
			
			m_DutyCycleInput.RegisterInputEvent([this](f32 value) {
				// Duty cycle validation - clamp to reasonable range
			});
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			m_SampleRate = sampleRate;
			m_Phase = 0.0;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update input parameters from connections
			m_FrequencyInput.UpdateFromConnections();
			m_PhaseInput.UpdateFromConnections();
			m_AmplitudeInput.UpdateFromConnections();
			m_DutyCycleInput.UpdateFromConnections();
			
			if (outputs && outputs[0])
			{
				// Audio stream processing mode - generate per-sample square wave
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Get current parameters for this sample
					f32 frequency = glm::clamp(m_FrequencyInput.GetValue(), MIN_FREQ_HZ, MAX_FREQ_HZ);
					f32 phaseOffset = m_PhaseInput.GetValue();
					f32 amplitude = m_AmplitudeInput.GetValue();
					f32 dutyCycle = glm::clamp(m_DutyCycleInput.GetValue(), 0.01f, 0.99f);
					
					// Calculate phase increment for this sample
					f64 phaseIncrement = frequency / m_SampleRate;
					
					// Generate square wave sample
					f64 currentPhase = std::fmod(m_Phase + static_cast<f64>(phaseOffset), 1.0);
					
					// Square wave: high if phase < dutyCycle, low otherwise
					f32 squareValue = (currentPhase < static_cast<f64>(dutyCycle)) ? 1.0f : -1.0f;
					
					outputs[0][i] = squareValue * amplitude;
					
					// Store output value in stream for this sample
					m_Output.SetValue(outputs[0][i]);
					
					// Advance phase
					m_Phase += phaseIncrement;
					if (m_Phase >= 1.0)
					{
						m_Phase -= 1.0;
					}
				}
			}
			else
			{
				// Single-value processing mode - generate single square sample
				f32 frequency = glm::clamp(m_FrequencyInput.GetValue(), MIN_FREQ_HZ, MAX_FREQ_HZ);
				f32 phaseOffset = m_PhaseInput.GetValue();
				f32 amplitude = m_AmplitudeInput.GetValue();
				f32 dutyCycle = glm::clamp(m_DutyCycleInput.GetValue(), 0.01f, 0.99f);
				
				f64 currentPhase = std::fmod(m_Phase + static_cast<f64>(phaseOffset), 1.0);
				
				f32 squareValue = (currentPhase < static_cast<f64>(dutyCycle)) ? 1.0f : -1.0f;
				f32 outputValue = squareValue * amplitude;
				
				m_Output.SetValue(outputValue);
				
				// Advance phase for next call
				f64 phaseIncrement = frequency / m_SampleRate;
				m_Phase += phaseIncrement * numSamples;
				if (m_Phase >= 1.0)
				{
					m_Phase = std::fmod(m_Phase, 1.0);
				}
			}

			// Update output connections with latest values
			m_Output.UpdateOutputConnections();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SquareNode");
		}

		const char* GetDisplayName() const override
		{
			return "Square Oscillator";
		}

		//==============================================================================
		/// Utility methods for external control and compatibility
		
		f32 GetCurrentFrequency() const { return m_FrequencyInput.GetValue(); }
		f64 GetCurrentPhase() const { return m_Phase; }
		void ResetPhase(f64 phase = 0.0) { m_Phase = phase; }
		f32 GetDutyCycle() const { return m_DutyCycleInput.GetValue(); }
		f32 GetOutput() const { return m_Output.GetValue(); }

		//==============================================================================
		/// Direct access methods for compatibility
		
		void SetFrequency(f32 frequency) { 
			m_FrequencyInput.SetValue(glm::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ)); 
		}
		void SetPhase(f32 phase) { m_PhaseInput.SetValue(phase); }
		void SetAmplitude(f32 amplitude) { m_AmplitudeInput.SetValue(amplitude); }
		void SetDutyCycle(f32 dutyCycle) { 
			m_DutyCycleInput.SetValue(glm::clamp(dutyCycle, 0.01f, 0.99f)); 
		}
	};

} // namespace OloEngine::Audio::SoundGraph