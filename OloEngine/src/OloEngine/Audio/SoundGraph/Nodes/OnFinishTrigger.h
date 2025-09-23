#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// OnFinishTrigger - Triggers when audio playback finishes
	/// Monitors audio source nodes and outputs trigger event when playback ends
	class OnFinishTrigger : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier Threshold_ID = OLO_IDENTIFIER("Threshold");

		// Node state
		bool m_LastPlayingState = false;
		f32 m_SilenceCounter = 0.0f;
		f64 m_SampleRate = 44100.0;
		Flag m_ResetFlag;

		// Output event
		std::shared_ptr<OutputEvent> m_OutputEvent;

	public:
		OnFinishTrigger()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);        // Audio input to monitor
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);        // Reset trigger
			AddParameter<f32>(Threshold_ID, "Threshold", 0.001f); // Audio detection threshold

			// Register input event for reset
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output event
			m_OutputEvent = AddOutputEvent<f32>(Output_ID, "Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			m_LastPlayingState = false;
			m_SilenceCounter = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for reset trigger
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				m_LastPlayingState = false;
				m_SilenceCounter = 0.0f;
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			// Get threshold for audio detection
			f32 threshold = GetParameterValue<f32>(Threshold_ID, 0.001f);
			f32 inputValue = GetParameterValue<f32>(Input_ID, 0.0f);
			bool currentlyPlaying = (std::abs(inputValue) > threshold);

			// Calculate frame time
			f32 frameTime = static_cast<f32>(numSamples) / static_cast<f32>(m_SampleRate);

			if (currentlyPlaying)
			{
				m_SilenceCounter = 0.0f;
				m_LastPlayingState = true;
			}
			else if (m_LastPlayingState)
			{
				// Accumulate silence time
				m_SilenceCounter += frameTime;
				
				// If we've had enough silence, consider playback finished
				// Use a small grace period (50ms) to avoid false triggers from brief silence
				if (m_SilenceCounter >= 0.05f)
				{
					// Trigger event when playback finishes
					if (m_OutputEvent)
						(*m_OutputEvent)(1.0f);
					
					m_LastPlayingState = false;
					m_SilenceCounter = 0.0f;
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("OnFinishTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "On Finish Trigger";
		}
	};
}