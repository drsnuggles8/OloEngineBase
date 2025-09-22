#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// RepeatTrigger - Generates periodic trigger events
	/// Based on Hazel's RepeatTrigger node
	class RepeatTrigger : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Period_ID = OLO_IDENTIFIER("Period");
		const Identifier Start_ID = OLO_IDENTIFIER("Start");
		const Identifier Stop_ID = OLO_IDENTIFIER("Stop");
		const Identifier IsPlaying_ID = OLO_IDENTIFIER("IsPlaying");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Node state
		bool m_Playing = false;
		f32 m_Counter = 0.0f;
		f64 m_SampleRate = 44100.0;

		// Flags for parameter change detection
		Flag m_StartFlag;
		Flag m_StopFlag;

		// Output event
		std::shared_ptr<OutputEvent> m_OutputEvent;

	public:
		RepeatTrigger()
		{
			// Register parameters
			AddParameter<f32>(Period_ID, "Period", 1.0f);
			AddParameter<f32>(Start_ID, "Start", 0.0f);
			AddParameter<f32>(Stop_ID, "Stop", 0.0f);
			AddParameter<f32>(IsPlaying_ID, "IsPlaying", 0.0f);

			// Register input events with flag callbacks
			AddInputEvent<f32>(Start_ID, "Start", [this](f32 value) {
				if (value > 0.5f) m_StartFlag.SetDirty();
			});
			AddInputEvent<f32>(Stop_ID, "Stop", [this](f32 value) {
				if (value > 0.5f) m_StopFlag.SetDirty();
			});

			// Register output event
			m_OutputEvent = AddOutputEvent<f32>(Output_ID, "Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for start/stop triggers via parameters or flags
			f32 startValue = GetParameterValue<f32>(Start_ID);
			f32 stopValue = GetParameterValue<f32>(Stop_ID);
			
			if (startValue > 0.5f || m_StartFlag.CheckAndResetIfDirty())
			{
				m_Playing = true;
				m_Counter = 0.0f;
				// Update IsPlaying parameter
				SetParameterValue(IsPlaying_ID, 1.0f);
				// Trigger immediately on start
				if (m_OutputEvent)
					(*m_OutputEvent)(1.0f);
				// Reset trigger parameter
				if (startValue > 0.5f)
					SetParameterValue(Start_ID, 0.0f);
			}

			if (stopValue > 0.5f || m_StopFlag.CheckAndResetIfDirty())
			{
				m_Playing = false;
				// Update IsPlaying parameter
				SetParameterValue(IsPlaying_ID, 0.0f);
				// Reset trigger parameter
				if (stopValue > 0.5f)
					SetParameterValue(Stop_ID, 0.0f);
			}

			if (!m_Playing)
				return;

			// Calculate frame time
			f32 frameTime = 1.0f / static_cast<f32>(m_SampleRate);

			for (u32 i = 0; i < numSamples; ++i)
			{
				m_Counter += frameTime;

				// Get current period value
				f32 period = GetParameterValue<f32>(Period_ID, 1.0f);

				if (period > 0.0f && m_Counter >= period)
				{
					if (m_OutputEvent)
						(*m_OutputEvent)(1.0f);
					
					m_Counter = 0.0f; // Reset counter after trigger
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("RepeatTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "Repeat Trigger";
		}
	};
}