#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// OnPlayTrigger - Triggers when audio playback starts
	/// Monitors audio source nodes and outputs trigger event when playback begins
	class OnPlayTrigger : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");

		// Node state
		bool m_LastPlayingState = false;
		Flag m_ResetFlag;

		// Output event
		std::shared_ptr<OutputEvent> m_OutputEvent;

	public:
		OnPlayTrigger()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);  // Audio input to monitor
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);  // Reset trigger

			// Register input event for reset
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output event
			m_OutputEvent = AddOutputEvent<f32>(Output_ID, "Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_LastPlayingState = false;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for reset trigger
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				m_LastPlayingState = false;
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			// Monitor input signal for audio activity
			f32 inputValue = GetParameterValue<f32>(Input_ID, 0.0f);
			bool currentlyPlaying = (inputValue > 0.001f); // Threshold for audio detection

			// Detect transition from not playing to playing
			if (currentlyPlaying && !m_LastPlayingState)
			{
				// Trigger event when playback starts
				if (m_OutputEvent)
					(*m_OutputEvent)(1.0f);
			}

			m_LastPlayingState = currentlyPlaying;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("OnPlayTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "On Play Trigger";
		}
	};
}