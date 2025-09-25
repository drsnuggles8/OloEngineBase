#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// OnFinishTrigger - Triggers when audio playback finishes
	/// Monitors audio source nodes and outputs trigger event when playback ends
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class OnFinishTrigger : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_InputView;
		InputView<f32> m_ResetView;
		InputView<f32> m_ThresholdView;
		
		// Output streams
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentReset = 0.0f;
		f32 m_CurrentThreshold = 0.001f; // Audio detection threshold
		f32 m_CurrentOutput = 0.0f;

		//======================================================================
		// Finish Detection State
		//======================================================================
		
		bool m_LastPlayingState = false;
		f32 m_SilenceCounter = 0.0f;
		f64 m_SampleRate = 44100.0;
		f32 m_PreviousReset = 0.0f;
		
		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;
		static constexpr f32 GRACE_PERIOD = 0.05f; // 50ms grace period to avoid false triggers

	public:
		OnFinishTrigger()
			: m_InputView([this](f32 value) { m_CurrentInput = value; }),
			  m_ResetView([this](f32 value) { m_CurrentReset = value; }),
			  m_ThresholdView([this](f32 value) { m_CurrentThreshold = value; }),
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
			m_ThresholdView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize state
			m_SampleRate = sampleRate;
			m_LastPlayingState = false;
			m_SilenceCounter = 0.0f;
			m_PreviousReset = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			m_ThresholdView.UpdateFromConnections(inputs, numSamples);
			
			// Calculate frame time for timing calculations
			f32 frameTime = static_cast<f32>(numSamples) / static_cast<f32>(m_SampleRate);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 inputValue = m_InputView.GetValue(sample);
				f32 resetValue = m_ResetView.GetValue(sample);
				f32 threshold = m_ThresholdView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				m_CurrentReset = resetValue;
				m_CurrentThreshold = threshold;
				
				// Detect reset edge (rising edge detection)
				bool resetEdge = resetValue > TRIGGER_THRESHOLD && m_PreviousReset <= TRIGGER_THRESHOLD;
				
				f32 outputTrigger = 0.0f;
				
				// Handle reset
				if (resetEdge)
				{
					m_LastPlayingState = false;
					m_SilenceCounter = 0.0f;
				}
				
				// Monitor input signal for audio activity
				bool currentlyPlaying = (std::abs(inputValue) > threshold);
				
				if (currentlyPlaying)
				{
					m_SilenceCounter = 0.0f;
					m_LastPlayingState = true;
				}
				else if (m_LastPlayingState)
				{
					// Accumulate silence time per sample
					f32 sampleTime = 1.0f / static_cast<f32>(m_SampleRate);
					m_SilenceCounter += sampleTime;
					
					// If we've had enough silence, consider playback finished
					if (m_SilenceCounter >= GRACE_PERIOD)
					{
						// Trigger event when playback finishes
						outputTrigger = 1.0f;
						m_LastPlayingState = false;
						m_SilenceCounter = 0.0f;
					}
				}
				
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
			return OLO_IDENTIFIER("OnFinishTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "On Finish Trigger";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Reset")) m_CurrentReset = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Threshold")) m_CurrentThreshold = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("Reset")) return static_cast<T>(m_CurrentReset);
			else if (id == OLO_IDENTIFIER("Threshold")) return static_cast<T>(m_CurrentThreshold);
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

		/// Get the current silence counter value
		f32 GetSilenceCounter() const
		{
			return m_SilenceCounter;
		}

		/// Get the current input level
		f32 GetInputLevel() const
		{
			return m_CurrentInput;
		}

		/// Manually reset the finish detection state
		void ManualReset()
		{
			m_LastPlayingState = false;
			m_SilenceCounter = 0.0f;
		}
	};

} // namespace OloEngine::Audio::SoundGraph