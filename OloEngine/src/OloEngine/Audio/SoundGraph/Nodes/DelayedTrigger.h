#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// DelayedTrigger - Delays trigger events by a configurable amount
	/// Based on Hazel's DelayedTrigger node
	class DelayedTrigger : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier DelayTime_ID = OLO_IDENTIFIER("DelayTime");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier DelayedOut_ID = OLO_IDENTIFIER("DelayedOut");
		const Identifier PassthroughOut_ID = OLO_IDENTIFIER("PassthroughOut");

		// Delay state
		bool m_WaitingToTrigger = false;
		f32 m_DelayCounter = 0.0f;
		f64 m_SampleRate = 44100.0;

		// Flags for parameter change detection
		Flag m_TriggerFlag;
		Flag m_ResetFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_DelayedOutEvent;
		std::shared_ptr<OutputEvent> m_PassthroughOutEvent;

	public:
		DelayedTrigger()
		{
			// Register parameters
			AddParameter<f32>(DelayTime_ID, "DelayTime", 0.5f);
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);

			// Register input events with flag callbacks
			AddInputEvent<f32>(Trigger_ID, "Trigger", [this](f32 value) {
				if (value > 0.5f) m_TriggerFlag.SetDirty();
			});
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output events
			m_DelayedOutEvent = AddOutputEvent<f32>(DelayedOut_ID, "DelayedOut");
			m_PassthroughOutEvent = AddOutputEvent<f32>(PassthroughOut_ID, "PassthroughOut");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for triggers via parameter or flag
			f32 triggerValue = GetParameterValue<f32>(Trigger_ID);
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			
			if (triggerValue > 0.5f || m_TriggerFlag.CheckAndResetIfDirty())
			{
				StartDelay();
				// Reset trigger parameter
				if (triggerValue > 0.5f)
					SetParameterValue(Trigger_ID, 0.0f);
			}

			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				CancelDelay();
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			if (!m_WaitingToTrigger)
				return;

			// Calculate frame time
			f32 frameTime = 1.0f / static_cast<f32>(m_SampleRate);

			for (u32 i = 0; i < numSamples; ++i)
			{
				m_DelayCounter += frameTime;

				// Get current delay time
				f32 delayTime = GetParameterValue<f32>(DelayTime_ID, 0.5f);

				if (m_DelayCounter >= delayTime)
				{
					// Fire delayed trigger
					if (m_DelayedOutEvent)
						(*m_DelayedOutEvent)(1.0f);
					
					m_WaitingToTrigger = false;
					m_DelayCounter = 0.0f;
					break; // Exit early since we're done
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("DelayedTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "Delayed Trigger";
		}

	private:
		void StartDelay()
		{
			// Fire passthrough immediately
			if (m_PassthroughOutEvent)
				(*m_PassthroughOutEvent)(1.0f);

			// Start delay countdown
			m_WaitingToTrigger = true;
			m_DelayCounter = 0.0f;
		}

		void CancelDelay()
		{
			m_WaitingToTrigger = false;
			m_DelayCounter = 0.0f;
		}
	};
}