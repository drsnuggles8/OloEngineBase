#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ThresholdTrigger - Triggers when input value crosses a threshold
	/// Supports both rising and falling edge detection with hysteresis
	class ThresholdTrigger : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Threshold_ID = OLO_IDENTIFIER("Threshold");
		const Identifier Hysteresis_ID = OLO_IDENTIFIER("Hysteresis");
		const Identifier Mode_ID = OLO_IDENTIFIER("Mode");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Node state
		bool m_LastState = false;
		Flag m_ResetFlag;

		// Output event
		std::shared_ptr<OutputEvent> m_OutputEvent;

	public:
		// Trigger modes
		enum class TriggerMode : i32
		{
			Rising = 0,   // Trigger when input rises above threshold
			Falling = 1,  // Trigger when input falls below threshold
			Both = 2      // Trigger on both rising and falling edges
		};

		ThresholdTrigger()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Threshold_ID, "Threshold", 0.5f);
			AddParameter<f32>(Hysteresis_ID, "Hysteresis", 0.01f);  // Small hysteresis to prevent noise
			AddParameter<i32>(Mode_ID, "Mode", static_cast<i32>(TriggerMode::Rising));
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);

			// Register input event for reset
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output event
			m_OutputEvent = AddOutputEvent<f32>(Output_ID, "Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_LastState = false;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for reset trigger
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				m_LastState = false;
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			// Get parameters
			f32 inputValue = GetParameterValue<f32>(Input_ID, 0.0f);
			f32 threshold = GetParameterValue<f32>(Threshold_ID, 0.5f);
			f32 hysteresis = GetParameterValue<f32>(Hysteresis_ID, 0.01f);
			TriggerMode mode = static_cast<TriggerMode>(GetParameterValue<i32>(Mode_ID, static_cast<i32>(TriggerMode::Rising)));

			// Calculate thresholds with hysteresis
			f32 upperThreshold = threshold + (hysteresis * 0.5f);
			f32 lowerThreshold = threshold - (hysteresis * 0.5f);

			// Determine current state with hysteresis
			bool currentState;
			if (m_LastState)
			{
				// If we were above threshold, need to fall below lower threshold to change state
				currentState = inputValue > lowerThreshold;
			}
			else
			{
				// If we were below threshold, need to rise above upper threshold to change state
				currentState = inputValue > upperThreshold;
			}

			// Check for state change and trigger accordingly
			bool shouldTrigger = false;
			
			if (currentState != m_LastState)
			{
				switch (mode)
				{
					case TriggerMode::Rising:
						shouldTrigger = currentState && !m_LastState;  // Rising edge
						break;
					case TriggerMode::Falling:
						shouldTrigger = !currentState && m_LastState;  // Falling edge
						break;
					case TriggerMode::Both:
						shouldTrigger = true;  // Any edge
						break;
				}

				if (shouldTrigger && m_OutputEvent)
				{
					(*m_OutputEvent)(1.0f);
				}
			}

			m_LastState = currentState;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ThresholdTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "Threshold Trigger";
		}
	};
}