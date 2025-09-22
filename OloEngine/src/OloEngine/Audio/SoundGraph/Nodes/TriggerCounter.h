#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// TriggerCounter - Counts trigger events and generates stepped values
	/// Based on Hazel's TriggerCounter node
	class TriggerCounter : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier StartValue_ID = OLO_IDENTIFIER("StartValue");
		const Identifier StepSize_ID = OLO_IDENTIFIER("StepSize");
		const Identifier ResetCount_ID = OLO_IDENTIFIER("ResetCount");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier Count_ID = OLO_IDENTIFIER("Count");
		const Identifier Value_ID = OLO_IDENTIFIER("Value");
		const Identifier CountOut_ID = OLO_IDENTIFIER("CountOut");
		const Identifier ValueOut_ID = OLO_IDENTIFIER("ValueOut");

		// Counter state
		i32 m_Count = 0;
		f32 m_LastStartValue = 0.0f;

		// Flags for parameter change detection
		Flag m_TriggerFlag;
		Flag m_ResetFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_CountOutEvent;
		std::shared_ptr<OutputEvent> m_ValueOutEvent;

	public:
		TriggerCounter()
		{
			// Register parameters
			AddParameter<f32>(StartValue_ID, "StartValue", 0.0f);
			AddParameter<f32>(StepSize_ID, "StepSize", 1.0f);
			AddParameter<f32>(ResetCount_ID, "ResetCount", 8.0f);
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);
			AddParameter<f32>(Count_ID, "Count", 0.0f);
			AddParameter<f32>(Value_ID, "Value", 0.0f);

			// Register input events with flag callbacks
			AddInputEvent<f32>(Trigger_ID, "Trigger", [this](f32 value) {
				if (value > 0.5f) m_TriggerFlag.SetDirty();
			});
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output events
			m_CountOutEvent = AddOutputEvent<i32>(CountOut_ID, "CountOut");
			m_ValueOutEvent = AddOutputEvent<f32>(ValueOut_ID, "ValueOut");
		}

		// Override SetParameterValue to handle StartValue changes
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			// Call base implementation
			NodeProcessor::SetParameterValue(id, value);
			
			// Handle StartValue changes when count is 0
			if (id == StartValue_ID && m_Count == 0)
			{
				f32 startValue = static_cast<f32>(value);
				NodeProcessor::SetParameterValue(Value_ID, startValue);
				NodeProcessor::SetParameterValue(ValueOut_ID, startValue);
				m_LastStartValue = startValue;
				
				if (m_ValueOutEvent)
					(*m_ValueOutEvent)(startValue);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			ResetCounter();
			// Update last start value to current parameter value
			m_LastStartValue = GetParameterValue<f32>(StartValue_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for StartValue changes and update the Value parameter accordingly
			f32 currentStartValue = GetParameterValue<f32>(StartValue_ID, 0.0f);
			if (currentStartValue != m_LastStartValue && m_Count == 0)
			{
				// Update initial value to reflect new StartValue
				SetParameterValue(Value_ID, currentStartValue);
				SetParameterValue(ValueOut_ID, currentStartValue);
				if (m_ValueOutEvent)
					(*m_ValueOutEvent)(currentStartValue);
				m_LastStartValue = currentStartValue;
			}

			// Check for triggers via parameter or flag
			f32 triggerValue = GetParameterValue<f32>(Trigger_ID);
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			
			if (triggerValue > 0.5f || m_TriggerFlag.CheckAndResetIfDirty())
			{
				IncrementCounter();
				// Reset trigger parameter
				if (triggerValue > 0.5f)
					SetParameterValue(Trigger_ID, 0.0f);
			}

			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				ResetCounter();
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("TriggerCounter");
		}

		const char* GetDisplayName() const override
		{
			return "Trigger Counter";
		}

	private:
		void IncrementCounter()
		{
			m_Count++;

			// Calculate current value: StartValue + StepSize * Count
			f32 startValue = GetParameterValue<f32>(StartValue_ID, 0.0f);
			f32 stepSize = GetParameterValue<f32>(StepSize_ID, 1.0f);
			f32 currentValue = startValue + stepSize * static_cast<f32>(m_Count);

			// Update parameters
			SetParameterValue(Count_ID, static_cast<f32>(m_Count));
			SetParameterValue(Value_ID, currentValue);

			// Update output parameters
			SetParameterValue(CountOut_ID, m_Count);
			SetParameterValue(ValueOut_ID, currentValue);

			// Fire output events
			if (m_CountOutEvent)
				(*m_CountOutEvent)(static_cast<f32>(m_Count));
			if (m_ValueOutEvent)
				(*m_ValueOutEvent)(currentValue);

			// Check for auto-reset AFTER updating the values
			f32 resetCount = GetParameterValue<f32>(ResetCount_ID, 8.0f);
			if (resetCount > 0.5f && static_cast<f32>(m_Count) >= resetCount)
			{
				ResetCounter();
			}
		}

		void ResetCounter()
		{
			m_Count = 0;

			// Calculate reset value using current StartValue parameter
			f32 startValue = GetParameterValue<f32>(StartValue_ID, 0.0f);
			m_LastStartValue = startValue;
			
			// Update parameters
			SetParameterValue(Count_ID, static_cast<f32>(m_Count));
			SetParameterValue(Value_ID, startValue);

			// Update output parameters
			SetParameterValue(CountOut_ID, m_Count);
			SetParameterValue(ValueOut_ID, startValue);

			// Fire output events
			if (m_CountOutEvent)
				(*m_CountOutEvent)(static_cast<f32>(m_Count));
			if (m_ValueOutEvent)
				(*m_ValueOutEvent)(startValue);
		}
	};
}