#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "../Events.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// AREnvelope - Attack-Release envelope generator
	/// Provides a simple two-phase envelope ideal for percussive sounds
	/// Features retrigger capability and velocity scaling
	class AREnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Release
		};

	private:
		// Endpoint identifiers
		const Identifier AttackTime_ID = OLO_IDENTIFIER("AttackTime");
		const Identifier ReleaseTime_ID = OLO_IDENTIFIER("ReleaseTime");
		const Identifier AttackCurve_ID = OLO_IDENTIFIER("AttackCurve");
		const Identifier ReleaseCurve_ID = OLO_IDENTIFIER("ReleaseCurve");
		const Identifier Peak_ID = OLO_IDENTIFIER("Peak");
		const Identifier Velocity_ID = OLO_IDENTIFIER("Velocity");
		const Identifier Retrigger_ID = OLO_IDENTIFIER("Retrigger");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Completed_ID = OLO_IDENTIFIER("Completed");

		// Envelope state
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_ReleaseSamples = 0;
		u32 m_CurrentSample = 0;
		f32 m_CurrentVelocity = 1.0f;

		// Events and flags
		Flag m_TriggerFlag;
		std::shared_ptr<OutputEvent> m_CompletedEvent;

	public:
		AREnvelope()
		{
			// Register parameters
			AddParameter<f32>(AttackTime_ID, "AttackTime", 0.01f);        // Attack time in seconds
			AddParameter<f32>(ReleaseTime_ID, "ReleaseTime", 0.2f);       // Release time in seconds
			AddParameter<f32>(AttackCurve_ID, "AttackCurve", 1.0f);       // Attack curve shaping
			AddParameter<f32>(ReleaseCurve_ID, "ReleaseCurve", 1.0f);     // Release curve shaping
			AddParameter<f32>(Peak_ID, "Peak", 1.0f);                     // Peak amplitude
			AddParameter<f32>(Velocity_ID, "Velocity", 1.0f);             // Note velocity (0-1)
			AddParameter<f32>(Retrigger_ID, "Retrigger", 1.0f);           // Allow retrigger (0 = off, 1 = on)
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);               // Trigger input
			AddParameter<f32>(Output_ID, "Output", 0.0f);                 // Envelope output

			// Set up trigger event
			AddInputEvent<f32>(Trigger_ID, "Trigger", 
				[this](f32 value) { 
					if (value > 0.5f) 
					{
						m_CurrentVelocity = GetParameterValue<f32>(Velocity_ID);
						m_TriggerFlag.SetDirty();
					}
				});

			// Set up completed event
			m_CompletedEvent = AddOutputEvent<f32>(Completed_ID, "Completed");
		}

		virtual ~AREnvelope() = default;

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
					m_CurrentVelocity = GetParameterValue<f32>(Velocity_ID);
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
			return OLO_IDENTIFIER("AREnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "AR Envelope";
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerEnvelope()
		{
			// Check retrigger capability
			if (m_CurrentState != State::Idle && GetParameterValue<f32>(Retrigger_ID) < 0.5f)
			{
				return; // Cannot retrigger
			}

			// Calculate envelope parameters
			f32 attackTime = glm::max(0.001f, GetParameterValue<f32>(AttackTime_ID));
			f32 releaseTime = glm::max(0.001f, GetParameterValue<f32>(ReleaseTime_ID));

			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_ReleaseSamples = static_cast<u32>(releaseTime * m_SampleRate);

			// Start envelope
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
		}

		void UpdateEnvelope()
		{
			const f32 peak = GetParameterValue<f32>(Peak_ID) * m_CurrentVelocity;
			const f32 attackCurve = glm::clamp(GetParameterValue<f32>(AttackCurve_ID), 0.1f, 10.0f);
			const f32 releaseCurve = glm::clamp(GetParameterValue<f32>(ReleaseCurve_ID), 0.1f, 10.0f);

			switch (m_CurrentState)
			{
			case State::Attack:
				if (m_CurrentSample < m_AttackSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					f32 curvedProgress = glm::pow(progress, 1.0f / attackCurve);
					m_CurrentValue = curvedProgress * peak;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = peak;
					m_CurrentState = State::Release;
					m_CurrentSample = 0;
				}
				break;

			case State::Release:
				if (m_CurrentSample < m_ReleaseSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_ReleaseSamples;
					f32 curvedProgress = glm::pow(progress, releaseCurve);
					m_CurrentValue = peak * (1.0f - curvedProgress);
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = 0.0f;
					m_CurrentState = State::Idle;
					
					// Fire completion event
					if (m_CompletedEvent)
					{
						(*m_CompletedEvent)(1.0f);
					}
				}
				break;

			case State::Idle:
				m_CurrentValue = 0.0f;
				break;
			}
		}

		void ResetEnvelope()
		{
			m_CurrentState = State::Idle;
			m_CurrentValue = 0.0f;
			m_CurrentSample = 0;
			m_CurrentVelocity = 1.0f;
		}

		// Utility methods for external access
		State GetCurrentState() const { return m_CurrentState; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
		f32 GetCurrentVelocity() const { return m_CurrentVelocity; }
		bool IsActive() const { return m_CurrentState != State::Idle; }
		bool CanRetrigger() const { return GetParameterValue<f32>(Retrigger_ID) > 0.5f; }
	};

} // namespace OloEngine::Audio::SoundGraph