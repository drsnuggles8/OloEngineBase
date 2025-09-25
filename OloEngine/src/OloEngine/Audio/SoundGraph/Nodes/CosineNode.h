#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// CosineNode - A cosine wave oscillator for audio synthesis
	/// Generates clean cosine waves with controllable frequency and phase
	/// Provides 90-degree phase shift from sine wave, useful for quadrature oscillators
	/// and stereo effects
	class CosineNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_FrequencyView;
		ValueView<f32> m_PhaseOffsetView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Oscillator State
		//======================================================================
		
		f32 m_CurrentFrequency = 440.0f;
		f32 m_CurrentPhaseOffset = 0.0f;
		
		// Oscillator phase accumulator
		f64 m_Phase = 0.0;

		// Frequency limits for audio safety
		static constexpr f32 MIN_FREQ_HZ = 0.0f;
		static constexpr f32 MAX_FREQ_HZ = 22000.0f;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit CosineNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_FrequencyView("Frequency", 440.0f)
			, m_PhaseOffsetView("PhaseOffset", 0.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Frequency", [this](f32 value) { m_CurrentFrequency = value; });
			RegisterInputEvent<f32>("PhaseOffset", [this](f32 value) { m_CurrentPhaseOffset = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_FrequencyView.Initialize(maxBufferSize);
			m_PhaseOffsetView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize phase
			m_Phase = static_cast<f64>(m_CurrentPhaseOffset);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			m_PhaseOffsetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 frequency = m_FrequencyView.GetValue(sample);
				f32 phaseOffset = m_PhaseOffsetView.GetValue(sample);
				
				// Update internal state
				m_CurrentFrequency = frequency;
				m_CurrentPhaseOffset = phaseOffset;
				
				// Clamp frequency to safe range
				frequency = std::clamp(frequency, MIN_FREQ_HZ, MAX_FREQ_HZ);
				
				// Calculate current phase with offset
				f64 currentPhase = m_Phase + static_cast<f64>(phaseOffset);
				
				// Generate cosine wave value
				f32 cosineValue = static_cast<f32>(std::cos(currentPhase));
				
				// Update phase accumulator
				f64 phaseIncrement = frequency * 2.0 * M_PI / m_SampleRate;
				m_Phase += phaseIncrement;
				
				// Wrap phase around 2π
				if (m_Phase >= 2.0 * M_PI)
				{
					m_Phase -= 2.0 * M_PI;
				}
				
				// Set output value
				m_OutputView.SetValue(sample, cosineValue);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("CosineNode");
		}

		const char* GetDisplayName() const override
		{
			return "Cosine Oscillator";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("PhaseOffset")) m_CurrentPhaseOffset = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
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
	};

} // namespace OloEngine::Audio::SoundGraph