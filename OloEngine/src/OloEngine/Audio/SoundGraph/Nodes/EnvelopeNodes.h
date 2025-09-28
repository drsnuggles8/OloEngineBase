#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

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
			// Check for parameter changes and recalculate rates if needed
			if (*in_AttackTime != m_CachedAttackTime ||
				*in_DecayTime != m_CachedDecayTime ||
				*in_AttackCurve != m_CachedAttackCurve ||
				*in_DecayCurve != m_CachedDecayCurve ||
				m_SampleRate != m_CachedSampleRate)
			{
				RecalculateRates();
				m_CachedAttackTime = *in_AttackTime;
				m_CachedDecayTime = *in_DecayTime;
				m_CachedAttackCurve = *in_AttackCurve;
				m_CachedDecayCurve = *in_DecayCurve;
				m_CachedSampleRate = m_SampleRate;
			}

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

		// Normalized progress tracking (0..1)
		float m_AttackProgress{ 0.0f };
		float m_DecayProgress{ 0.0f };

		Flag m_TriggerFlag;

		// Cached parameter values for runtime change detection
		f32 m_CachedAttackTime = -1.0f;
		f32 m_CachedDecayTime = -1.0f;
		f32 m_CachedAttackCurve = -1.0f;
		f32 m_CachedDecayCurve = -1.0f;
		f32 m_CachedSampleRate = -1.0f;

		void RecalculateRates()
		{
			m_AttackCurve = glm::max(0.1f, *in_AttackCurve);
			m_DecayCurve = glm::max(0.1f, *in_DecayCurve);

			// Calculate attack rate (exponential coefficient for reaching 0.99)
			if (*in_AttackTime <= 0.0f)
				m_AttackRate = 1.0f; // Immediate
			else
			{
				const float remainingRatio = 0.01f; // Reach 0.99 of target
				m_AttackRate = 1.0f - powf(remainingRatio, 1.0f / (*in_AttackTime * m_SampleRate));
			}

			// Calculate decay rate (exponential coefficient for reaching 0.01)
			if (*in_DecayTime <= 0.0f)
				m_DecayRate = 1.0f; // Immediate
			else
			{
				const float remainingRatio = 0.01f; // Reach 0.01 of start value
				m_DecayRate = 1.0f - powf(remainingRatio, 1.0f / (*in_DecayTime * m_SampleRate));
			}
		}

		void StartAttack()
		{
			m_State = Attack;
			m_Target = 1.0f;
			m_AttackProgress = 0.0f; // Reset attack progress
			out_OnTrigger(1.0f);
		}

		void ProcessAttack()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_AttackProgress += m_AttackRate;
			m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress
			float curvedProgress = glm::pow(m_AttackProgress, 1.0f / m_AttackCurve);
			
			// Interpolate using curved progress
			m_Value = curvedProgress; // Attack goes from 0 to 1

			// Check if attack is complete
			if (m_AttackProgress >= 1.0f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_Target = 0.0f;
				m_DecayProgress = 0.0f; // Reset decay progress
			}
		}

		void ProcessDecay()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_DecayProgress += m_DecayRate;
			m_DecayProgress = glm::clamp(m_DecayProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress
			float curvedProgress = glm::pow(m_DecayProgress, 1.0f / m_DecayCurve);
			
			// Interpolate using curved progress (decay goes from 1 to 0)
			m_Value = 1.0f - curvedProgress;

			// Check if decay is complete
			if (m_DecayProgress >= 1.0f)
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
			AddInEvent(IDs::Trigger, [this](float v) { (void)v; m_TriggerFlag.SetDirty(); });
			AddInEvent(IDs::Release, [this](float v) { (void)v; m_ReleaseFlag.SetDirty(); });

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
			// Check for parameter changes and recalculate rates if needed
			if (*in_AttackTime != m_CachedAttackTime ||
				*in_DecayTime != m_CachedDecayTime ||
				*in_ReleaseTime != m_CachedReleaseTime ||
				*in_AttackCurve != m_CachedAttackCurve ||
				*in_DecayCurve != m_CachedDecayCurve ||
				*in_ReleaseCurve != m_CachedReleaseCurve ||
				m_SampleRate != m_CachedSampleRate)
			{
				RecalculateRates();
				m_CachedAttackTime = *in_AttackTime;
				m_CachedDecayTime = *in_DecayTime;
				m_CachedReleaseTime = *in_ReleaseTime;
				m_CachedAttackCurve = *in_AttackCurve;
				m_CachedDecayCurve = *in_DecayCurve;
				m_CachedReleaseCurve = *in_ReleaseCurve;
				m_CachedSampleRate = m_SampleRate;
			}

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

		// Normalized progress tracking (0..1)
		float m_AttackProgress{ 0.0f };
		float m_DecayProgress{ 0.0f };
		float m_ReleaseProgress{ 0.0f };

		Flag m_TriggerFlag;
		Flag m_ReleaseFlag;

		// Cached parameter values for runtime change detection
		f32 m_CachedAttackTime = -1.0f;
		f32 m_CachedDecayTime = -1.0f;
		f32 m_CachedReleaseTime = -1.0f;
		f32 m_CachedAttackCurve = -1.0f;
		f32 m_CachedDecayCurve = -1.0f;
		f32 m_CachedReleaseCurve = -1.0f;
		f32 m_CachedSampleRate = -1.0f;

		void RecalculateRates()
		{
			m_AttackCurve = glm::max(0.1f, *in_AttackCurve);
			m_DecayCurve = glm::max(0.1f, *in_DecayCurve);
			m_ReleaseCurve = glm::max(0.1f, *in_ReleaseCurve);

			// Calculate per-sample progress increments (1/durationInSamples)
			if (*in_AttackTime <= 0.0f)
				m_AttackRate = 1.0f; // Immediate (complete in one sample)
			else
				m_AttackRate = 1.0f / (*in_AttackTime * m_SampleRate);

			if (*in_DecayTime <= 0.0f)
				m_DecayRate = 1.0f; // Immediate
			else
				m_DecayRate = 1.0f / (*in_DecayTime * m_SampleRate);

			if (*in_ReleaseTime <= 0.0f)
				m_ReleaseRate = 1.0f; // Immediate
			else
				m_ReleaseRate = 1.0f / (*in_ReleaseTime * m_SampleRate);
		}

		void StartAttack()
		{
			m_State = Attack;
			m_Target = 1.0f;
			m_AttackProgress = 0.0f; // Reset attack progress
			out_OnTrigger(1.0f);
		}

		void StartRelease()
		{
			if (m_State != Idle && m_State != Release)
			{
				m_State = Release;
				m_SustainStartValue = m_Value;
				m_Target = 0.0f;
				m_ReleaseProgress = 0.0f; // Reset release progress
				out_OnRelease(1.0f);
			}
		}

		void ProcessAttack()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_AttackProgress += m_AttackRate;
			m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress
			float curvedProgress = glm::pow(m_AttackProgress, 1.0f / m_AttackCurve);
			
			// Interpolate using curved progress (attack goes from 0 to 1)
			m_Value = curvedProgress;

			// Check if attack is complete
			if (m_AttackProgress >= 1.0f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_Target = glm::clamp(*in_SustainLevel, 0.0f, 1.0f);
				m_DecayProgress = 0.0f; // Reset decay progress
			}
		}

		void ProcessDecay()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_DecayProgress += m_DecayRate;
			m_DecayProgress = glm::clamp(m_DecayProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress
			float curvedProgress = glm::pow(m_DecayProgress, 1.0f / m_DecayCurve);
			
			// Interpolate using curved progress (decay from 1.0 to sustain level)
			float sustainLevel = glm::clamp(*in_SustainLevel, 0.0f, 1.0f);
			m_Value = 1.0f - curvedProgress * (1.0f - sustainLevel);

			// Check if decay is complete
			if (m_DecayProgress >= 1.0f)
			{
				m_Value = sustainLevel;
				m_State = Sustain;
			}
		}

		void ProcessRelease()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_ReleaseProgress += m_ReleaseRate;
			m_ReleaseProgress = glm::clamp(m_ReleaseProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress
			float curvedProgress = glm::pow(m_ReleaseProgress, 1.0f / m_ReleaseCurve);
			
			// Interpolate using curved progress (release from sustain level to 0)
			m_Value = m_SustainStartValue * (1.0f - curvedProgress);

			// Check if release is complete
			if (m_ReleaseProgress >= 1.0f)
			{
				m_Value = 0.0f;
				m_State = Idle;
				out_OnComplete(1.0f);
			}
		}
	};
}

#undef DECLARE_ID