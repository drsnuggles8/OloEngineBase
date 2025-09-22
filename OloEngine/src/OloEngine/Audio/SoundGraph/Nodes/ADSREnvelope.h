#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "../Events.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ADSREnvelope - Classic Attack-Decay-Sustain-Release envelope generator
	/// Provides full ADSR envelope with note-on/note-off control
	/// Essential for musical instruments and expressive sound design
	class ADSREnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Decay,
			Sustain,
			Release
		};

	private:
		// Endpoint identifiers
		const Identifier AttackTime_ID = OLO_IDENTIFIER("AttackTime");
		const Identifier DecayTime_ID = OLO_IDENTIFIER("DecayTime");
		const Identifier SustainLevel_ID = OLO_IDENTIFIER("SustainLevel");
		const Identifier ReleaseTime_ID = OLO_IDENTIFIER("ReleaseTime");
		const Identifier AttackCurve_ID = OLO_IDENTIFIER("AttackCurve");
		const Identifier DecayCurve_ID = OLO_IDENTIFIER("DecayCurve");
		const Identifier ReleaseCurve_ID = OLO_IDENTIFIER("ReleaseCurve");
		const Identifier Peak_ID = OLO_IDENTIFIER("Peak");
		const Identifier Velocity_ID = OLO_IDENTIFIER("Velocity");
		const Identifier NoteOn_ID = OLO_IDENTIFIER("NoteOn");
		const Identifier NoteOff_ID = OLO_IDENTIFIER("NoteOff");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier StateOutput_ID = OLO_IDENTIFIER("StateOutput");

		// Envelope state
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		f32 m_TargetValue = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_DecaySamples = 0;
		u32 m_ReleaseSamples = 0;
		u32 m_CurrentSample = 0;
		f32 m_StartValue = 0.0f;
		f32 m_CurrentVelocity = 1.0f;

		// Events and flags
		Flag m_NoteOnFlag;
		Flag m_NoteOffFlag;

	public:
		ADSREnvelope()
		{
			// Register parameters
			AddParameter<f32>(AttackTime_ID, "AttackTime", 0.01f);        // Attack time in seconds
			AddParameter<f32>(DecayTime_ID, "DecayTime", 0.1f);           // Decay time in seconds
			AddParameter<f32>(SustainLevel_ID, "SustainLevel", 0.7f);     // Sustain level (0-1)
			AddParameter<f32>(ReleaseTime_ID, "ReleaseTime", 0.3f);       // Release time in seconds
			AddParameter<f32>(AttackCurve_ID, "AttackCurve", 1.0f);       // Attack curve shaping
			AddParameter<f32>(DecayCurve_ID, "DecayCurve", 1.0f);         // Decay curve shaping
			AddParameter<f32>(ReleaseCurve_ID, "ReleaseCurve", 1.0f);     // Release curve shaping
			AddParameter<f32>(Peak_ID, "Peak", 1.0f);                     // Peak amplitude
			AddParameter<f32>(Velocity_ID, "Velocity", 1.0f);             // Note velocity (0-1)
			AddParameter<f32>(NoteOn_ID, "NoteOn", 0.0f);                 // Note on trigger
			AddParameter<f32>(NoteOff_ID, "NoteOff", 0.0f);               // Note off trigger
			AddParameter<f32>(Output_ID, "Output", 0.0f);                 // Envelope output
			AddParameter<f32>(StateOutput_ID, "StateOutput", 0.0f);       // Current state as float

			// Set up note events
			AddInputEvent<f32>(NoteOn_ID, "NoteOn", 
				[this](f32 value) { 
					if (value > 0.5f) 
					{
						m_CurrentVelocity = GetParameterValue<f32>(Velocity_ID);
						m_NoteOnFlag.SetDirty();
					}
				});

			AddInputEvent<f32>(NoteOff_ID, "NoteOff", 
				[this](f32 value) { 
					if (value > 0.5f) m_NoteOffFlag.SetDirty(); 
				});
		}

		virtual ~ADSREnvelope() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Check for note events via parameters (for direct testing) or flags (for event-based)
				f32 noteOnValue = GetParameterValue<f32>(NoteOn_ID);
				f32 noteOffValue = GetParameterValue<f32>(NoteOff_ID);
				
				if (noteOnValue > 0.5f || m_NoteOnFlag.CheckAndResetIfDirty())
				{
					m_CurrentVelocity = GetParameterValue<f32>(Velocity_ID);
					TriggerNoteOn();
					// Reset note-on parameter after processing
					if (noteOnValue > 0.5f)
					{
						SetParameterValue(NoteOn_ID, 0.0f);
					}
				}
				
				if (noteOffValue > 0.5f || m_NoteOffFlag.CheckAndResetIfDirty())
				{
					TriggerNoteOff();
					// Reset note-off parameter after processing
					if (noteOffValue > 0.5f)
					{
						SetParameterValue(NoteOff_ID, 0.0f);
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

			// Set output parameters to last generated values
			SetParameterValue(Output_ID, m_CurrentValue);
			SetParameterValue(StateOutput_ID, static_cast<f32>(m_CurrentState));
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			ResetEnvelope();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ADSREnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "ADSR Envelope";
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerNoteOn()
		{
			// Calculate envelope parameters
			f32 attackTime = glm::max(0.001f, GetParameterValue<f32>(AttackTime_ID));
			f32 decayTime = glm::max(0.001f, GetParameterValue<f32>(DecayTime_ID));

			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_DecaySamples = static_cast<u32>(decayTime * m_SampleRate);

			// Start attack phase
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
			m_StartValue = m_CurrentValue; // Start from current value for smooth transitions
		}

		void TriggerNoteOff()
		{
			if (m_CurrentState != State::Idle)
			{
				f32 releaseTime = glm::max(0.001f, GetParameterValue<f32>(ReleaseTime_ID));
				m_ReleaseSamples = static_cast<u32>(releaseTime * m_SampleRate);

				m_CurrentState = State::Release;
				m_CurrentSample = 0;
				m_StartValue = m_CurrentValue; // Start release from current value
			}
		}

		void UpdateEnvelope()
		{
			const f32 peak = GetParameterValue<f32>(Peak_ID) * m_CurrentVelocity;
			const f32 sustainLevel = GetParameterValue<f32>(SustainLevel_ID) * peak;
			const f32 attackCurve = glm::clamp(GetParameterValue<f32>(AttackCurve_ID), 0.1f, 10.0f);
			const f32 decayCurve = glm::clamp(GetParameterValue<f32>(DecayCurve_ID), 0.1f, 10.0f);
			const f32 releaseCurve = glm::clamp(GetParameterValue<f32>(ReleaseCurve_ID), 0.1f, 10.0f);

			switch (m_CurrentState)
			{
			case State::Attack:
				if (m_CurrentSample < m_AttackSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					f32 curvedProgress = glm::pow(progress, 1.0f / attackCurve);
					m_CurrentValue = m_StartValue + (peak - m_StartValue) * curvedProgress;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = peak;
					m_CurrentState = State::Decay;
					m_CurrentSample = 0;
					m_StartValue = peak;
				}
				break;

			case State::Decay:
				if (m_CurrentSample < m_DecaySamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_DecaySamples;
					f32 curvedProgress = glm::pow(progress, decayCurve);
					m_CurrentValue = peak + (sustainLevel - peak) * curvedProgress;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = sustainLevel;
					m_CurrentState = State::Sustain;
				}
				break;

			case State::Sustain:
				m_CurrentValue = sustainLevel;
				break;

			case State::Release:
				if (m_CurrentSample < m_ReleaseSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_ReleaseSamples;
					f32 curvedProgress = glm::pow(progress, releaseCurve);
					m_CurrentValue = m_StartValue * (1.0f - curvedProgress);
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = 0.0f;
					m_CurrentState = State::Idle;
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
			m_StartValue = 0.0f;
			m_CurrentVelocity = 1.0f;
		}

		// Utility methods for external access
		State GetCurrentState() const { return m_CurrentState; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
		f32 GetCurrentVelocity() const { return m_CurrentVelocity; }
		bool IsActive() const { return m_CurrentState != State::Idle; }
		bool IsInSustain() const { return m_CurrentState == State::Sustain; }
	};

} // namespace OloEngine::Audio::SoundGraph