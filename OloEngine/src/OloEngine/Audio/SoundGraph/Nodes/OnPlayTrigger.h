#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// OnPlayTrigger - Triggers when audio playback starts
	/// Monitors audio source nodes and outputs trigger event when playback begins
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class OnPlayTrigger : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_InputView;
		InputView<f32> m_ResetView;
		
		// Output streams
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentReset = 0.0f;
		f32 m_CurrentOutput = 0.0f;

		//======================================================================
		// Play Detection State
		//======================================================================
		
		bool m_LastPlayingState = false;
		f32 m_PreviousReset = 0.0f;
		
		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;
		static constexpr f32 AUDIO_THRESHOLD = 0.001f; // Threshold for audio detection

	public:
		OnPlayTrigger()
			: m_InputView([this](f32 value) { m_CurrentInput = value; }),
			  m_ResetView([this](f32 value) { m_CurrentReset = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize state
			m_LastPlayingState = false;
			m_PreviousReset = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 inputValue = m_InputView.GetValue(sample);
				f32 resetValue = m_ResetView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				m_CurrentReset = resetValue;
				
				// Detect reset edge (rising edge detection)
				bool resetEdge = resetValue > TRIGGER_THRESHOLD && m_PreviousReset <= TRIGGER_THRESHOLD;
				
				f32 outputTrigger = 0.0f;
				
				// Handle reset
				if (resetEdge)
				{
					m_LastPlayingState = false;
				}
				
				// Monitor input signal for audio activity
				bool currentlyPlaying = (inputValue > AUDIO_THRESHOLD);
				
				// Detect transition from not playing to playing
				if (currentlyPlaying && !m_LastPlayingState)
				{
					// Trigger event when playback starts
					outputTrigger = 1.0f;
				}
				
				m_LastPlayingState = currentlyPlaying;
				
				// Update output
				m_CurrentOutput = outputTrigger;
				m_OutputView.SetValue(sample, outputTrigger);
				
				// Update previous values for edge detection
				m_PreviousReset = resetValue;
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("OnPlayTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "On Play Trigger";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Reset")) m_CurrentReset = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("Reset")) return static_cast<T>(m_CurrentReset);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentOutput);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Check if currently detecting playback
		bool IsPlayingDetected() const
		{
			return m_LastPlayingState;
		}

		/// Get the current input level
		f32 GetInputLevel() const
		{
			return m_CurrentInput;
		}

		/// Manually reset the play detection state
		void ManualReset()
		{
			m_LastPlayingState = false;
		}
	};

} // namespace OloEngine::Audio::SoundGraph