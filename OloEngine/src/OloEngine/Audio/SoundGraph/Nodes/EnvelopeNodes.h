#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	// Attack-Decay Envelope Generator
	//==============================================================================
	struct ADEnvelope : public NodeProcessor
	{
		struct IDs
		{
			DECLARE_ID(Trigger);
		private:
			IDs() = delete;
		};

		explicit ADEnvelope(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			// Input events
			AddInEvent(IDs::Trigger, [this](float v) { m_TriggerFlag.SetDirty(); });

			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();

			// Sample rate is now set by NodeProcessor base class
			RecalculateRates();
			
			// Initialize state
			m_Value = 0.0f;
			m_Target = 0.0f;
			m_State = Idle;
		}

		void Process() final
		{
			// Handle trigger events
			if (m_TriggerFlag.IsDirty())
			{
				m_TriggerFlag.CheckAndResetIfDirty();
				StartAttack();
			}

			// Process envelope state
			switch (m_State)
			{
			case Idle:
				// Do nothing, value remains at 0
				break;

			case Attack:
				ProcessAttack();
				break;

			case Decay:
				ProcessDecay();
				break;
			}

			out_OutEnvelope = m_Value;
		}

		// Input parameters
		float* in_AttackTime = nullptr;		// Attack time in seconds
		float* in_DecayTime = nullptr;		// Decay time in seconds
		float* in_AttackCurve = nullptr;	// Attack curve shaping (1.0 = linear, >1 = convex, <1 = concave)
		float* in_DecayCurve = nullptr;		// Decay curve shaping
		bool* in_Looping = nullptr;			// Enable looping (retrigger after decay)

		// Outputs
		float out_OutEnvelope{ 0.0f };

		// Output events
		OutputEvent out_OnTrigger{ *this };
		OutputEvent out_OnComplete{ *this };

		void RegisterEndpoints();
		void InitializeInputs();

	private:
		enum State
		{
			Idle = 0,		// Before attack / after decay
			Attack = 1,		// Rising phase
			Decay = 2,		// Falling phase
		};

		State m_State{ Idle };
		float m_Value{ 0.0f };
		float m_Target{ 0.0f };

		// Pre-calculated rates
		float m_AttackRate{ 0.001f };
		float m_DecayRate{ 0.001f };
		float m_AttackCurve{ 1.0f };
		float m_DecayCurve{ 1.0f };

		Flag m_TriggerFlag;

		void RecalculateRates()
		{
			m_AttackCurve = glm::max(0.1f, *in_AttackCurve);
			m_DecayCurve = glm::max(0.1f, *in_DecayCurve);

			// Calculate attack rate (per sample)
			if (*in_AttackTime <= 0.0f)
				m_AttackRate = 1.0f; // Immediate
			else
				m_AttackRate = 1.0f / (*in_AttackTime * m_SampleRate);

			// Calculate decay rate (per sample)
			if (*in_DecayTime <= 0.0f)
				m_DecayRate = 1.0f; // Immediate
			else
				m_DecayRate = 1.0f / (*in_DecayTime * m_SampleRate);
		}

		void StartAttack()
		{
			m_State = Attack;
			m_Target = 1.0f;
			out_OnTrigger(1.0f);
		}

		void ProcessAttack()
		{
			// Exponential curve for attack
			float progress = m_AttackRate;
			float curvedProgress = glm::pow(progress, 1.0f / m_AttackCurve);
			
			m_Value += curvedProgress * (m_Target - m_Value);

			// Check if attack is complete (within small threshold)
			if (m_Value >= 0.99f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_Target = 0.0f;
			}
		}

		void ProcessDecay()
		{
			// Exponential curve for decay
			float progress = m_DecayRate;
			float curvedProgress = glm::pow(progress, 1.0f / m_DecayCurve);
			
			m_Value += curvedProgress * (m_Target - m_Value);

			// Check if decay is complete
			if (m_Value <= 0.01f)
			{
				m_Value = 0.0f;
				m_State = Idle;
				out_OnComplete(1.0f);

				// Handle looping
				if (*in_Looping)
				{
					StartAttack();
				}
			}
		}
	};

	//==============================================================================
	// Attack-Decay-Sustain-Release Envelope Generator
	//==============================================================================
	struct ADSREnvelope : public NodeProcessor
	{
		struct IDs
		{
			DECLARE_ID(Trigger);
			DECLARE_ID(Release);
		private:
			IDs() = delete;
		};

		explicit ADSREnvelope(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			// Input events
			AddInEvent(IDs::Trigger, [this](float v) { m_TriggerFlag.SetDirty(); });
			AddInEvent(IDs::Release, [this](float v) { m_ReleaseFlag.SetDirty(); });

			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();

			// Sample rate is now set by NodeProcessor base class
			RecalculateRates();
			
			// Initialize state
			m_Value = 0.0f;
			m_Target = 0.0f;
			m_State = Idle;
		}

		void Process() final
		{
			// Handle trigger and release events
			if (m_TriggerFlag.IsDirty())
			{
				m_TriggerFlag.CheckAndResetIfDirty();
				StartAttack();
			}

			if (m_ReleaseFlag.IsDirty())
			{
				m_ReleaseFlag.CheckAndResetIfDirty();
				StartRelease();
			}

			// Process envelope state
			switch (m_State)
			{
			case Idle:
				// Value remains at 0
				break;

			case Attack:
				ProcessAttack();
				break;

			case Decay:
				ProcessDecay();
				break;

			case Sustain:
				// Value remains at sustain level
				m_Value = *in_SustainLevel;
				break;

			case Release:
				ProcessRelease();
				break;
			}

			out_OutEnvelope = m_Value;
		}

		// Input parameters
		float* in_AttackTime = nullptr;		// Attack time in seconds
		float* in_DecayTime = nullptr;		// Decay time in seconds
		float* in_SustainLevel = nullptr;	// Sustain level (0.0 to 1.0)
		float* in_ReleaseTime = nullptr;	// Release time in seconds
		float* in_AttackCurve = nullptr;	// Attack curve shaping
		float* in_DecayCurve = nullptr;		// Decay curve shaping
		float* in_ReleaseCurve = nullptr;	// Release curve shaping

		// Outputs
		float out_OutEnvelope{ 0.0f };

		// Output events
		OutputEvent out_OnTrigger{ *this };
		OutputEvent out_OnRelease{ *this };
		OutputEvent out_OnComplete{ *this };

		void RegisterEndpoints();
		void InitializeInputs();

	private:
		enum State
		{
			Idle = 0,		// Before trigger / after release
			Attack = 1,		// Rising to peak
			Decay = 2,		// Falling to sustain
			Sustain = 3,	// Holding sustain level
			Release = 4,	// Falling to zero
		};

		State m_State{ Idle };
		float m_Value{ 0.0f };
		float m_Target{ 0.0f };
		float m_SustainStartValue{ 0.0f }; // Value when release started

		// Pre-calculated rates
		float m_AttackRate{ 0.001f };
		float m_DecayRate{ 0.001f };
		float m_ReleaseRate{ 0.001f };
		float m_AttackCurve{ 1.0f };
		float m_DecayCurve{ 1.0f };
		float m_ReleaseCurve{ 1.0f };

		Flag m_TriggerFlag;
		Flag m_ReleaseFlag;

		void RecalculateRates()
		{
			m_AttackCurve = glm::max(0.1f, *in_AttackCurve);
			m_DecayCurve = glm::max(0.1f, *in_DecayCurve);
			m_ReleaseCurve = glm::max(0.1f, *in_ReleaseCurve);

			// Calculate rates (per sample)
			m_AttackRate = (*in_AttackTime <= 0.0f) ? 1.0f : 1.0f / (*in_AttackTime * m_SampleRate);
			m_DecayRate = (*in_DecayTime <= 0.0f) ? 1.0f : 1.0f / (*in_DecayTime * m_SampleRate);
			m_ReleaseRate = (*in_ReleaseTime <= 0.0f) ? 1.0f : 1.0f / (*in_ReleaseTime * m_SampleRate);
		}

		void StartAttack()
		{
			m_State = Attack;
			m_Target = 1.0f;
			out_OnTrigger(1.0f);
		}

		void StartRelease()
		{
			if (m_State != Idle && m_State != Release)
			{
				m_State = Release;
				m_SustainStartValue = m_Value;
				m_Target = 0.0f;
				out_OnRelease(1.0f);
			}
		}

		void ProcessAttack()
		{
			float progress = m_AttackRate;
			float curvedProgress = glm::pow(progress, 1.0f / m_AttackCurve);
			
			m_Value += curvedProgress * (m_Target - m_Value);

			if (m_Value >= 0.99f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_Target = glm::clamp(*in_SustainLevel, 0.0f, 1.0f);
			}
		}

		void ProcessDecay()
		{
			float progress = m_DecayRate;
			float curvedProgress = glm::pow(progress, 1.0f / m_DecayCurve);
			
			m_Value += curvedProgress * (m_Target - m_Value);

			// Check if decay reached sustain level (within threshold)
			float sustainLevel = glm::clamp(*in_SustainLevel, 0.0f, 1.0f);
			if (glm::abs(m_Value - sustainLevel) <= 0.01f)
			{
				m_Value = sustainLevel;
				m_State = Sustain;
			}
		}

		void ProcessRelease()
		{
			float progress = m_ReleaseRate;
			float curvedProgress = glm::pow(progress, 1.0f / m_ReleaseCurve);
			
			m_Value += curvedProgress * (m_Target - m_Value);

			if (m_Value <= 0.01f)
			{
				m_Value = 0.0f;
				m_State = Idle;
				out_OnComplete(1.0f);
			}
		}
	};
}

#undef DECLARE_ID