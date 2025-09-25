#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SawtoothNode - Generates sawtooth wave oscillation using new Hazel-style foundation
	/// Supports both rising and falling sawtooth waves with controllable parameters
	class SawtoothNode : public NodeProcessor
	{
	private:
		// Input streams for connected values
		ValueView<f32> m_FrequencyStream;
		ValueView<f32> m_PhaseStream;
		ValueView<f32> m_AmplitudeStream;
		ValueView<f32> m_DirectionStream;
		
		// Output stream for audio
		ValueView<f32> m_OutputStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentFrequency = 440.0f;
		f32 m_CurrentPhase = 0.0f;
		f32 m_CurrentAmplitude = 1.0f;
		f32 m_CurrentDirection = 1.0f;  // 1 = rising saw, -1 = falling saw
		f32 m_CurrentOutput = 0.0f;

		// Internal oscillator state
		f64 m_Phase = 0.0;
		f64 m_SampleRate = 44100.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		SawtoothNode()
		{
			// Create input events for receiving parameter values
			auto frequencyEvent = std::make_shared<InputEvent>("Frequency", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentFrequency = glm::clamp(value.Get<f32>(), MIN_FREQ_HZ, MAX_FREQ_HZ);
				}
			});
			
			auto phaseEvent = std::make_shared<InputEvent>("Phase", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentPhase = value.Get<f32>();
				}
			});

			auto amplitudeEvent = std::make_shared<InputEvent>("Amplitude", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentAmplitude = value.Get<f32>();
				}
			});

			auto directionEvent = std::make_shared<InputEvent>("Direction", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentDirection = value.Get<f32>() >= 0.0f ? 1.0f : -1.0f;  // Normalize to +1/-1
				}
			});

			// Register input events
			AddInputEvent(frequencyEvent);
			AddInputEvent(phaseEvent);
			AddInputEvent(amplitudeEvent);
			AddInputEvent(directionEvent);

			// Create output event for sending audio samples
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			AddOutputEvent(outputEvent);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			m_SampleRate = sampleRate;
			m_Phase = 0.0;
			
			// Initialize ValueView streams for real-time processing
			m_FrequencyStream = CreateValueView<f32>();
			m_PhaseStream = CreateValueView<f32>();
			m_AmplitudeStream = CreateValueView<f32>();
			m_DirectionStream = CreateValueView<f32>();
			m_OutputStream = CreateValueView<f32>();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			if (outputs && outputs[0])
			{
				// Audio stream processing mode - generate per-sample sawtooth wave
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Get current parameters (could be stream or single values)
					f32 frequency = m_FrequencyStream.HasStream() ? m_FrequencyStream.GetNextValue() : m_CurrentFrequency;
					f32 phaseOffset = m_PhaseStream.HasStream() ? m_PhaseStream.GetNextValue() : m_CurrentPhase;
					f32 amplitude = m_AmplitudeStream.HasStream() ? m_AmplitudeStream.GetNextValue() : m_CurrentAmplitude;
					f32 direction = m_DirectionStream.HasStream() ? m_DirectionStream.GetNextValue() : m_CurrentDirection;
					
					// Clamp frequency to safe range
					frequency = glm::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
					
					// Calculate phase increment
					f64 phaseIncrement = frequency / m_SampleRate;
					
					// Generate sawtooth wave sample
					f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
					currentPhase = std::fmod(currentPhase, 1.0); // Keep in [0, 1] range
					
					f32 sawtoothValue;
					if (direction >= 0.0f)
					{
						// Rising sawtooth: 0 to 1
						sawtoothValue = static_cast<f32>(currentPhase * 2.0 - 1.0); // Convert to [-1, 1]
					}
					else
					{
						// Falling sawtooth: 1 to 0  
						sawtoothValue = static_cast<f32>((1.0 - currentPhase) * 2.0 - 1.0); // Convert to [-1, 1]
					}
					
					outputs[0][i] = sawtoothValue * amplitude;
					
					// Advance phase
					m_Phase += phaseIncrement;
					if (m_Phase >= 1.0)
					{
						m_Phase -= 1.0;
					}
				}
				
				// Store last generated value
				m_CurrentOutput = outputs[0][numSamples - 1];
			}
			else
			{
				// Single-value processing mode - generate single sawtooth sample
				f64 currentPhase = m_Phase + static_cast<f64>(m_CurrentPhase);
				currentPhase = std::fmod(currentPhase, 1.0);
				
				f32 sawtoothValue;
				if (m_CurrentDirection >= 0.0f)
				{
					sawtoothValue = static_cast<f32>(currentPhase * 2.0 - 1.0);
				}
				else
				{
					sawtoothValue = static_cast<f32>((1.0 - currentPhase) * 2.0 - 1.0);
				}
				
				m_CurrentOutput = sawtoothValue * m_CurrentAmplitude;
				
				// Advance phase for next call
				f64 phaseIncrement = m_CurrentFrequency / m_SampleRate;
				m_Phase += phaseIncrement * numSamples;
				if (m_Phase >= 1.0)
				{
					m_Phase = std::fmod(m_Phase, 1.0);
				}
			}

			// Send output value via event system
			auto outputEvent = FindOutputEvent("Output");
			if (outputEvent)
			{
				Value outputValue = CreateValue<f32>(m_CurrentOutput);
				outputEvent->TriggerEvent(outputValue);
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SawtoothNode");
		}

		const char* GetDisplayName() const override
		{
			return "Sawtooth Oscillator";
		}

		//==============================================================================
		/// Utility methods for external control
		
		f32 GetCurrentFrequency() const { return m_CurrentFrequency; }
		f64 GetCurrentPhase() const { return m_Phase; }
		void ResetPhase(f64 phase = 0.0) { m_Phase = phase; }
		f32 GetDirection() const { return m_CurrentDirection; }

		//==============================================================================
		/// Direct access methods for compatibility
		
		void SetFrequency(f32 frequency) { m_CurrentFrequency = glm::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ); }
		void SetPhase(f32 phase) { m_CurrentPhase = phase; }
		void SetAmplitude(f32 amplitude) { m_CurrentAmplitude = amplitude; }
		void SetDirection(f32 direction) { m_CurrentDirection = direction >= 0.0f ? 1.0f : -1.0f; }
		f32 GetOutput() const { return m_CurrentOutput; }
	};

} // namespace OloEngine::Audio::SoundGraph