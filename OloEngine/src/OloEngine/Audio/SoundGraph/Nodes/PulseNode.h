#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// PulseNode - A pulse wave oscillator with variable duty cycle (PWM)
	/// Generates rectangular pulse waves with controllable pulse width
	/// Essential for classic synthesizer sounds and pulse width modulation effects
	class PulseNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_FrequencyView;
		ValueView<f32> m_PulseWidthView;
		ValueView<f32> m_PhaseOffsetView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Oscillator State
		//======================================================================
		
		f32 m_CurrentFrequency = 440.0f;
		f32 m_CurrentPulseWidth = 0.5f;
		f32 m_CurrentPhaseOffset = 0.0f;
		
		// Oscillator phase accumulator
		f64 m_Phase = 0.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

		// Pulse width limits (0.0 = 0%, 1.0 = 100%)
		static constexpr f32 MIN_PULSE_WIDTH = 0.001f;  // Prevent completely silent output
		static constexpr f32 MAX_PULSE_WIDTH = 0.999f;  // Prevent DC offset

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit PulseNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_FrequencyView("Frequency", 440.0f)
			, m_PulseWidthView("PulseWidth", 0.5f)
			, m_PhaseOffsetView("PhaseOffset", 0.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Frequency", [this](f32 value) { m_CurrentFrequency = value; });
			RegisterInputEvent<f32>("PulseWidth", [this](f32 value) { m_CurrentPulseWidth = value; });
			RegisterInputEvent<f32>("PhaseOffset", [this](f32 value) { m_CurrentPhaseOffset = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_FrequencyView.Initialize(maxBufferSize);
			m_PulseWidthView.Initialize(maxBufferSize);
			m_PhaseOffsetView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize phase
			m_Phase = 0.0;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			m_PulseWidthView.UpdateFromConnections(inputs, numSamples);
			m_PhaseOffsetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 frequency = m_FrequencyView.GetValue(sample);
				f32 pulseWidth = m_PulseWidthView.GetValue(sample);
				f32 phaseOffset = m_PhaseOffsetView.GetValue(sample);
				
				// Update internal state
				m_CurrentFrequency = frequency;
				m_CurrentPulseWidth = pulseWidth;
				m_CurrentPhaseOffset = phaseOffset;
				
				// Clamp frequency to safe range
				frequency = std::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
				// Clamp pulse width to prevent DC offset and silence
				pulseWidth = std::clamp(pulseWidth, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
				
				// Add phase offset and normalize to [0, 1] range
				f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
				f64 normalizedPhase = std::fmod(currentPhase, 2.0 * M_PI) / (2.0 * M_PI);
				if (normalizedPhase < 0.0) normalizedPhase += 1.0;
				
				// Generate pulse wave: +1.0 when phase < pulseWidth, -1.0 otherwise
				f32 pulseValue = (normalizedPhase < static_cast<f64>(pulseWidth)) ? 1.0f : -1.0f;
				
				// Update phase accumulator
				f64 phaseIncrement = frequency * 2.0 * M_PI / m_SampleRate;
				m_Phase += phaseIncrement;
				
				// Wrap phase around 2π
				while (m_Phase >= 2.0 * M_PI)
				{
					m_Phase -= 2.0 * M_PI;
				}
				
				// Set output value
				m_OutputView.SetValue(sample, pulseValue);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("PulseNode");
		}

		const char* GetDisplayName() const override
		{
			return "Pulse/PWM Oscillator";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("PulseWidth")) m_CurrentPulseWidth = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("PhaseOffset")) m_CurrentPhaseOffset = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			else if (id == OLO_IDENTIFIER("PulseWidth")) return static_cast<T>(m_CurrentPulseWidth);
			else if (id == OLO_IDENTIFIER("PhaseOffset")) return static_cast<T>(m_CurrentPhaseOffset);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_OutputView.GetCurrentValue());
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Reset the oscillator phase to the current offset
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
			return std::clamp(m_CurrentFrequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
		}

		/// Get the current pulse width (clamped to safe range)
		f32 GetCurrentPulseWidth() const
		{
			return std::clamp(m_CurrentPulseWidth, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
		}

		/// Set pulse width with validation
		void SetPulseWidth(f32 width)
		{
			m_CurrentPulseWidth = std::clamp(width, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
		}

		/// Get valid pulse width range
		static std::pair<f32, f32> GetPulseWidthRange()
		{
			return { MIN_PULSE_WIDTH, MAX_PULSE_WIDTH };
		}
	};

} // namespace OloEngine::Audio::SoundGraph