#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SineNode - A sine wave oscillator for audio synthesis using new Hazel-style foundation
	/// Generates clean sine waves with controllable frequency and phase
	/// Essential building block for audio synthesis and signal generation
	class SineNode : public NodeProcessor
	{
	private:
		// Input streams for connected values
		ValueView<f32> m_FrequencyStream;
		ValueView<f32> m_PhaseOffsetStream;
		
		// Output stream for audio
		ValueView<f32> m_OutputStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentFrequency = 440.0f;  // A4
		f32 m_CurrentPhaseOffset = 0.0f;
		f32 m_CurrentOutput = 0.0f;

		// Oscillator state
		f64 m_Phase = 0.0;
		f64 m_PhaseIncrement = 0.0;
		f64 m_SampleRate = 48000.0;

		// Reset phase trigger state
		bool m_ResetPhaseTrigger = false;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		SineNode()
		{
			// Create input events for receiving parameter values
			auto frequencyEvent = std::make_shared<InputEvent>("Frequency", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentFrequency = glm::clamp(value.Get<f32>(), MIN_FREQ_HZ, MAX_FREQ_HZ);
				}
			});
			
			auto phaseOffsetEvent = std::make_shared<InputEvent>("PhaseOffset", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentPhaseOffset = value.Get<f32>();
				}
			});

			auto resetPhaseEvent = std::make_shared<InputEvent>("ResetPhase", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32 && value.Get<f32>() > 0.5f)
				{
					m_ResetPhaseTrigger = true;
				}
			});

			// Register input events
			AddInputEvent(frequencyEvent);
			AddInputEvent(phaseOffsetEvent);
			AddInputEvent(resetPhaseEvent);

			// Create output event for sending audio samples
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			AddOutputEvent(outputEvent);
		}

		virtual ~SineNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			m_SampleRate = sampleRate;
			
			// Initialize ValueView streams for real-time processing
			m_FrequencyStream = CreateValueView<f32>();
			m_PhaseOffsetStream = CreateValueView<f32>();
			m_OutputStream = CreateValueView<f32>();
			
			// Initialize phase and calculate initial phase increment
			m_Phase = static_cast<f64>(m_CurrentPhaseOffset);
			m_PhaseIncrement = m_CurrentFrequency * glm::two_pi<f64>() / m_SampleRate;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for reset phase trigger
			if (m_ResetPhaseTrigger)
			{
				ResetPhase();
				m_ResetPhaseTrigger = false;
			}

			if (outputs && outputs[0])
			{
				// Audio stream processing mode - generate per-sample sine wave
				for (u32 i = 0; i < numSamples; ++i)
				{
					// Get current parameters (could be stream or single values)
					f32 frequency = m_FrequencyStream.HasStream() ? m_FrequencyStream.GetNextValue() : m_CurrentFrequency;
					f32 phaseOffset = m_PhaseOffsetStream.HasStream() ? m_PhaseOffsetStream.GetNextValue() : m_CurrentPhaseOffset;
					
					// Clamp frequency to safe range
					frequency = glm::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
					
					// Calculate phase increment for this sample
					m_PhaseIncrement = frequency * glm::two_pi<f64>() / m_SampleRate;
					
					// Generate sine wave sample
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
				
				// Store last generated value
				m_CurrentOutput = outputs[0][numSamples - 1];
			}
			else
			{
				// Single-value processing mode - generate single sine sample
				const f64 currentPhase = m_Phase + static_cast<f64>(m_CurrentPhaseOffset);
				m_CurrentOutput = static_cast<f32>(glm::sin(currentPhase));
				
				// Advance phase for next call
				m_PhaseIncrement = m_CurrentFrequency * glm::two_pi<f64>() / m_SampleRate;
				m_Phase += m_PhaseIncrement * numSamples;
				if (m_Phase >= glm::two_pi<f64>())
				{
					m_Phase = fmod(m_Phase, glm::two_pi<f64>());
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
			m_Phase = static_cast<f64>(m_CurrentPhaseOffset);
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
			return glm::clamp(m_CurrentFrequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
		}

		//==============================================================================
		/// Direct access methods for compatibility
		
		void SetFrequency(f32 frequency) { m_CurrentFrequency = glm::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ); }
		void SetPhaseOffset(f32 phaseOffset) { m_CurrentPhaseOffset = phaseOffset; }
		void TriggerResetPhase() { m_ResetPhaseTrigger = true; }
		f32 GetOutput() const { return m_CurrentOutput; }
	};

} // namespace OloEngine::Audio::SoundGraph