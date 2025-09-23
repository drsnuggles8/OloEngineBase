#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SampleAndHoldNode - Samples and holds an input value when triggered
	/// Useful for creating stepped sequences or randomized control values
	class SampleAndHoldNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier TriggerOut_ID = OLO_IDENTIFIER("TriggerOut");

		// Internal state
		f32 m_HeldValue = 0.0f;
		Flag m_TriggerFlag;
		Flag m_ResetFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_TriggerOutEvent;

	public:
		SampleAndHoldNode()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);
			AddParameter<f32>(Output_ID, "Output", 0.0f);

			// Register input events with flag callbacks
			AddInputEvent<f32>(Trigger_ID, "Trigger", [this](f32 value) {
				if (value > 0.5f) m_TriggerFlag.SetDirty();
			});
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output events
			m_TriggerOutEvent = AddOutputEvent<f32>(TriggerOut_ID, "TriggerOut");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_HeldValue = 0.0f;
			SetParameterValue(Output_ID, m_HeldValue);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for reset flag
			f32 resetValue = GetParameterValue<f32>(Reset_ID, 0.0f);
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				m_HeldValue = 0.0f;
				SetParameterValue(Output_ID, m_HeldValue);
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			// Check for trigger flag
			f32 triggerValue = GetParameterValue<f32>(Trigger_ID, 0.0f);
			if (triggerValue > 0.5f || m_TriggerFlag.CheckAndResetIfDirty())
			{
				SampleInput();
				// Reset trigger parameter
				if (triggerValue > 0.5f)
					SetParameterValue(Trigger_ID, 0.0f);
			}

			// Fill output buffer with held value if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = m_HeldValue;
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SampleAndHoldNode");
		}

		const char* GetDisplayName() const override
		{
			return "Sample & Hold";
		}

		// Utility methods
		f32 GetHeldValue() const
		{
			return m_HeldValue;
		}

		void SetHeldValue(f32 value)
		{
			m_HeldValue = value;
			SetParameterValue(Output_ID, m_HeldValue);
		}

		void ResetHold()
		{
			m_HeldValue = 0.0f;
			SetParameterValue(Output_ID, m_HeldValue);
		}

	private:
		void SampleInput()
		{
			// Sample the current input value
			f32 inputValue = GetParameterValue<f32>(Input_ID, 0.0f);
			m_HeldValue = inputValue;
			SetParameterValue(Output_ID, m_HeldValue);

			// Fire trigger output event
			if (m_TriggerOutEvent)
				(*m_TriggerOutEvent)(1.0f);
		}
	};
}