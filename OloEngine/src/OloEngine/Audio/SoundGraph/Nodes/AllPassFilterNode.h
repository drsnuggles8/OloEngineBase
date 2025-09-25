#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// AllPassFilterNode - Two-pole all-pass filter
	/// Passes all frequencies without amplitude change but alters phase relationships
	/// Essential for reverb algorithms, stereo widening, and phase manipulation effects
	class AllPassFilterNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_InputView;
		ValueView<f32> m_FrequencyView;
		ValueView<f32> m_ResonanceView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Filter State
		//======================================================================
		
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentFrequency = 1000.0f;
		f32 m_CurrentResonance = 1.0f;
		
		// Biquad filter state
		f32 m_PreviousOutput = 0.0f;
		f32 m_PreviousOutput2 = 0.0f;
		f32 m_PreviousInput = 0.0f;
		f32 m_PreviousInput2 = 0.0f;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit AllPassFilterNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputView("Input", 0.0f)
			, m_FrequencyView("Frequency", 1000.0f)
			, m_ResonanceView("Resonance", 1.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Input", [this](f32 value) { m_CurrentInput = value; });
			RegisterInputEvent<f32>("Frequency", [this](f32 value) { m_CurrentFrequency = value; });
			RegisterInputEvent<f32>("Resonance", [this](f32 value) { m_CurrentResonance = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_FrequencyView.Initialize(maxBufferSize);
			m_ResonanceView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Reset filter state
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			m_ResonanceView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 inputSample = m_InputView.GetValue(sample);
				f32 frequency = m_FrequencyView.GetValue(sample);
				f32 resonance = m_ResonanceView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputSample;
				m_CurrentFrequency = frequency;
				m_CurrentResonance = resonance;
				
				// Clamp frequency to reasonable range (avoid aliasing)
				frequency = std::clamp(frequency, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
				// Clamp resonance to avoid instability
				resonance = std::clamp(resonance, 0.1f, 10.0f);
				
				// Calculate biquad coefficients for all-pass filter
				f32 omega = 2.0f * M_PI * frequency / static_cast<f32>(m_SampleRate);
				f32 alpha = std::sin(omega) / (2.0f * resonance);
				
				f32 cos_omega = std::cos(omega);
				
				// All-pass filter coefficients
				f32 b0 = 1.0f - alpha;
				f32 b1 = -2.0f * cos_omega;
				f32 b2 = 1.0f + alpha;
				f32 a0 = 1.0f + alpha;
				f32 a1 = -2.0f * cos_omega;
				f32 a2 = 1.0f - alpha;

				// Normalize coefficients
				b0 /= a0;
				b1 /= a0;
				b2 /= a0;
				a1 /= a0;
				a2 /= a0;
				
				// Biquad filter implementation
				f32 output = b0 * inputSample + b1 * m_PreviousInput + b2 * m_PreviousInput2
						   - a1 * m_PreviousOutput - a2 * m_PreviousOutput2;
				
				// Update filter state
				m_PreviousInput2 = m_PreviousInput;
				m_PreviousInput = inputSample;
				m_PreviousOutput2 = m_PreviousOutput;
				m_PreviousOutput = output;
				
				// Set output value
				m_OutputView.SetValue(sample, output);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("AllPassFilterNode");
		}

		const char* GetDisplayName() const override
		{
			return "All-Pass Filter";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Resonance")) m_CurrentResonance = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			else if (id == OLO_IDENTIFIER("Resonance")) return static_cast<T>(m_CurrentResonance);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_OutputView.GetCurrentValue());
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Get the current characteristic frequency (clamped to safe range)
		f32 GetFrequency() const
		{
			return std::clamp(m_CurrentFrequency, 20.0f, static_cast<f32>(m_SampleRate * 0.45));
		}

		/// Get the current resonance factor
		f32 GetResonance() const
		{
			return std::clamp(m_CurrentResonance, 0.1f, 10.0f);
		}

		/// Reset the filter state to prevent audio artifacts
		void ResetFilter()
		{
			m_PreviousOutput = 0.0f;
			m_PreviousOutput2 = 0.0f;
			m_PreviousInput = 0.0f;
			m_PreviousInput2 = 0.0f;
		}

		/// Check if the filter preserves amplitude (should always be true for all-pass)
		bool PreservesAmplitude() const
		{
			return true; // All-pass filters by definition preserve amplitude
		}
	};

} // namespace OloEngine::Audio::SoundGraph