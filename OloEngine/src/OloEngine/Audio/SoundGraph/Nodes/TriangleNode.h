#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// TriangleNode - Generates triangle wave oscillation
	/// Based on Hazel's oscillator patterns
	class TriangleNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_FrequencyView;
		ValueView<f32> m_PhaseView;
		ValueView<f32> m_AmplitudeView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Oscillator State
		//======================================================================
		
		f32 m_CurrentFrequency = 440.0f;
		f32 m_CurrentPhase = 0.0f;
		f32 m_CurrentAmplitude = 1.0f;
		
		// Internal phase accumulator
		f64 m_Phase = 0.0;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit TriangleNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_FrequencyView("Frequency", 440.0f)
			, m_PhaseView("Phase", 0.0f)
			, m_AmplitudeView("Amplitude", 1.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Frequency", [this](f32 value) { m_CurrentFrequency = value; });
			RegisterInputEvent<f32>("Phase", [this](f32 value) { m_CurrentPhase = value; });
			RegisterInputEvent<f32>("Amplitude", [this](f32 value) { m_CurrentAmplitude = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_FrequencyView.Initialize(maxBufferSize);
			m_PhaseView.Initialize(maxBufferSize);
			m_AmplitudeView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Reset phase
			m_Phase = 0.0;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			m_PhaseView.UpdateFromConnections(inputs, numSamples);
			m_AmplitudeView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 frequency = m_FrequencyView.GetValue(sample);
				f32 phaseOffset = m_PhaseView.GetValue(sample);
				f32 amplitude = m_AmplitudeView.GetValue(sample);
				
				// Update internal state
				m_CurrentFrequency = frequency;
				m_CurrentPhase = phaseOffset;
				m_CurrentAmplitude = amplitude;
				
				// Clamp frequency to reasonable range
				frequency = std::clamp(frequency, 0.1f, static_cast<f32>(m_SampleRate * 0.5));
				
				// Calculate triangle wave: 2 * |2 * (phase - floor(phase + 0.5)) | - 1
				f64 normalizedPhase = m_Phase + static_cast<f64>(phaseOffset) / (2.0 * M_PI);
				normalizedPhase = normalizedPhase - std::floor(normalizedPhase); // Keep in [0, 1)

				f32 triangleValue;
				if (normalizedPhase < 0.5)
				{
					// Rising edge: 0 to 1
					triangleValue = static_cast<f32>(4.0 * normalizedPhase - 1.0);
				}
				else
				{
					// Falling edge: 1 to -1
					triangleValue = static_cast<f32>(3.0 - 4.0 * normalizedPhase);
				}

				f32 result = triangleValue * amplitude;
				
				// Update phase
				m_Phase += static_cast<f64>(frequency) / m_SampleRate;
				if (m_Phase >= 1.0)
					m_Phase -= 1.0;
				
				// Set output value
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("TriangleNode");
		}

		const char* GetDisplayName() const override
		{
			return "Triangle Oscillator";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Phase")) m_CurrentPhase = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Amplitude")) m_CurrentAmplitude = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			else if (id == OLO_IDENTIFIER("Phase")) return static_cast<T>(m_CurrentPhase);
			else if (id == OLO_IDENTIFIER("Amplitude")) return static_cast<T>(m_CurrentAmplitude);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_OutputView.GetCurrentValue());
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Get current frequency (clamped to safe range)
		f32 GetCurrentFrequency() const
		{
			return std::clamp(m_CurrentFrequency, 0.1f, static_cast<f32>(m_SampleRate * 0.5));
		}

		/// Get current phase accumulator value
		f64 GetCurrentPhase() const
		{
			return m_Phase;
		}

		/// Reset the phase accumulator
		void ResetPhase(f64 phase = 0.0)
		{
			m_Phase = phase;
		}

		/// Set frequency with validation
		void SetFrequency(f32 freq)
		{
			m_CurrentFrequency = std::clamp(freq, 0.1f, static_cast<f32>(m_SampleRate * 0.5));
		}

		/// Set amplitude with validation
		void SetAmplitude(f32 amp)
		{
			m_CurrentAmplitude = amp;
		}
	};

} // namespace OloEngine::Audio::SoundGraph