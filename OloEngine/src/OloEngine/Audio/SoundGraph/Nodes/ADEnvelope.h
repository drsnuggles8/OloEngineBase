#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "../Events.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ADEnvelope - Attack-Decay envelope generator
	/// Provides a simple two-phase envelope with trigger capability
	/// Ideal for percussive sounds and basic dynamics control
	class ADEnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Decay
		};

	private:
		// Endpoint identifiers
		const Identifier AttackTime_ID = OLO_IDENTIFIER("AttackTime");
		const Identifier DecayTime_ID = OLO_IDENTIFIER("DecayTime");
		const Identifier AttackCurve_ID = OLO_IDENTIFIER("AttackCurve");
		const Identifier DecayCurve_ID = OLO_IDENTIFIER("DecayCurve");
		const Identifier Peak_ID = OLO_IDENTIFIER("Peak");
		const Identifier Sustain_ID = OLO_IDENTIFIER("Sustain");
		const Identifier Loop_ID = OLO_IDENTIFIER("Loop");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Completed_ID = OLO_IDENTIFIER("Completed");

		// Envelope state
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		f32 m_AttackRate = 0.0f;
		f32 m_DecayRate = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_DecaySamples = 0;
		u32 m_CurrentSample = 0;

		// Events and flags
		Flag m_TriggerFlag;
		std::shared_ptr<OutputEvent> m_CompletedEvent;

	public:
		ADEnvelope()
		{
			// Register parameters
			AddParameter<f32>(AttackTime_ID, "AttackTime", 0.01f);    // Attack time in seconds
			AddParameter<f32>(DecayTime_ID, "DecayTime", 0.3f);       // Decay time in seconds
			AddParameter<f32>(AttackCurve_ID, "AttackCurve", 1.0f);   // Attack curve (0.1 = log, 1.0 = linear, 10.0 = exp)
			AddParameter<f32>(DecayCurve_ID, "DecayCurve", 1.0f);     // Decay curve (0.1 = log, 1.0 = linear, 10.0 = exp)
			AddParameter<f32>(Peak_ID, "Peak", 1.0f);                 // Peak amplitude
			AddParameter<f32>(Sustain_ID, "Sustain", 0.0f);           // Sustain level
			AddParameter<f32>(Loop_ID, "Loop", 0.0f);                 // Loop enable (0 = off, 1 = on)
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);           // Trigger input
			AddParameter<f32>(Output_ID, "Output", 0.0f);             // Envelope output

			// Set up trigger event
			AddInputEvent<f32>(Trigger_ID, "Trigger", 
				[this](f32 value) { 
					if (value > 0.5f) m_TriggerFlag.SetDirty(); 
				});

			// Set up completed event
			m_CompletedEvent = AddOutputEvent<f32>(Completed_ID, "Completed");
		}

		virtual ~ADEnvelope() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Check for trigger via parameter (for direct testing) or flag (for event-based)
				f32 triggerValue = GetParameterValue<f32>(Trigger_ID);
				if (triggerValue > 0.5f || m_TriggerFlag.CheckAndResetIfDirty())
				{
					TriggerEnvelope();
					// Reset trigger parameter after processing
					if (triggerValue > 0.5f)
					{
						SetParameterValue(Trigger_ID, 0.0f);
					}
				}

				// Update envelope based on current state
				UpdateEnvelope();

				// Write to output buffer if available
				if (outputs && outputs[0])
				{
					outputs[0][sample] = m_CurrentValue;
				}
			}

			// Set output parameter to last generated value
			SetParameterValue(Output_ID, m_CurrentValue);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			ResetEnvelope();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ADEnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "AD Envelope";
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerEnvelope()
		{
			// Calculate envelope parameters
			f32 attackTime = glm::max(0.001f, GetParameterValue<f32>(AttackTime_ID));
			f32 decayTime = glm::max(0.001f, GetParameterValue<f32>(DecayTime_ID));

			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_DecaySamples = static_cast<u32>(decayTime * m_SampleRate);

			// Calculate rates for linear progression
			m_AttackRate = m_AttackSamples > 0 ? 1.0f / m_AttackSamples : 1.0f;
			m_DecayRate = m_DecaySamples > 0 ? 1.0f / m_DecaySamples : 1.0f;

			// Start envelope
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
		}

		void UpdateEnvelope()
		{
			const f32 peak = GetParameterValue<f32>(Peak_ID);
			const f32 sustain = GetParameterValue<f32>(Sustain_ID);
			const f32 attackCurve = glm::clamp(GetParameterValue<f32>(AttackCurve_ID), 0.1f, 10.0f);
			const f32 decayCurve = glm::clamp(GetParameterValue<f32>(DecayCurve_ID), 0.1f, 10.0f);

			switch (m_CurrentState)
			{
			case State::Attack:
				if (m_CurrentSample < m_AttackSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					f32 curvedProgress = glm::pow(progress, attackCurve);
					m_CurrentValue = curvedProgress * peak;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = peak;
					m_CurrentState = State::Decay;
					m_CurrentSample = 0;
				}
				break;

			case State::Decay:
				if (m_CurrentSample < m_DecaySamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_DecaySamples;
					f32 curvedProgress = glm::pow(progress, decayCurve);
					m_CurrentValue = peak + (sustain - peak) * curvedProgress;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = sustain;
					
					// Check for loop
					if (GetParameterValue<f32>(Loop_ID) > 0.5f)
					{
						TriggerEnvelope();
					}
					else
					{
						m_CurrentState = State::Idle;
						if (m_CompletedEvent)
						{
							(*m_CompletedEvent)(1.0f);
						}
					}
				}
				break;

			case State::Idle:
				m_CurrentValue = sustain;
				break;
			}
		}

		void ResetEnvelope()
		{
			m_CurrentState = State::Idle;
			m_CurrentValue = GetParameterValue<f32>(Sustain_ID);
			m_CurrentSample = 0;
		}

		// Utility methods for external access
		State GetCurrentState() const { return m_CurrentState; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
		bool IsActive() const { return m_CurrentState != State::Idle; }
	};

} // namespace OloEngine::Audio::SoundGraph