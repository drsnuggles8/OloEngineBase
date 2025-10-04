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
			AddInEvent(IDs::Trigger, [this](float v) { (void)v; m_TriggerFlag.SetDirty(); });

			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();

			// Sample rate is now set by NodeProcessor base class
			RecalculateRates();
			
			// Initialize state
			m_Value = 0.0f;
			m_State = Idle;
		}

		void Process() final
		{
			OLO_PROFILE_FUNCTION();

			// Check for parameter changes and recalculate rates if needed
			if (*m_InAttackTime != m_CachedAttackTime ||
				*m_InDecayTime != m_CachedDecayTime ||
				*m_InAttackCurve != m_CachedAttackCurve ||
				*m_InDecayCurve != m_CachedDecayCurve ||
				m_SampleRate != m_CachedSampleRate)
			{
				RecalculateRates();
				m_CachedAttackTime = *m_InAttackTime;
				m_CachedDecayTime = *m_InDecayTime;
				m_CachedAttackCurve = *m_InAttackCurve;
				m_CachedDecayCurve = *m_InDecayCurve;
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

			m_OutOutEnvelope = m_Value;
		}

		// Input parameters
		f32* m_InAttackTime = nullptr;		// Attack time in seconds
		f32* m_InDecayTime = nullptr;		// Decay time in seconds
		f32* m_InAttackCurve = nullptr;	// Attack curve shaping (1.0 = linear, >1 = convex, <1 = concave)
		f32* m_InDecayCurve = nullptr;		// Decay curve shaping
		bool* m_InLooping = nullptr;			// Enable looping (retrigger after decay)

		// Outputs
		f32 m_OutOutEnvelope{ 0.0f };

		// Output events
		OutputEvent m_OutOnTrigger{ *this };
		OutputEvent m_OutOnComplete{ *this };

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
		f32 m_Value{ 0.0f };

		// Pre-calculated rates
		f32 m_AttackRate{ 0.001f };
		f32 m_DecayRate{ 0.001f };
		f32 m_AttackCurve{ 1.0f };
		f32 m_DecayCurve{ 1.0f };

		// Normalized progress tracking (0..1)
		f32 m_AttackProgress{ 0.0f };
		f32 m_DecayProgress{ 0.0f };

		Flag m_TriggerFlag;

		// Cached parameter values for runtime change detection
		f32 m_CachedAttackTime = -1.0f;
		f32 m_CachedDecayTime = -1.0f;
		f32 m_CachedAttackCurve = -1.0f;
		f32 m_CachedDecayCurve = -1.0f;
		f32 m_CachedSampleRate = -1.0f;

		void RecalculateRates()
		{
			m_AttackCurve = glm::max(0.1f, *m_InAttackCurve);
			m_DecayCurve = glm::max(0.1f, *m_InDecayCurve);

			// Calculate attack rate (linear per-sample increment to reach 1.0 in specified time)
			if (*m_InAttackTime <= 0.0f)
				m_AttackRate = 1.0f; // Immediate
			else
				m_AttackRate = 1.0f / (*m_InAttackTime * m_SampleRate);

			// Calculate decay rate (linear per-sample increment to reach 1.0 in specified time)
			if (*m_InDecayTime <= 0.0f)
				m_DecayRate = 1.0f; // Immediate
			else
				m_DecayRate = 1.0f / (*m_InDecayTime * m_SampleRate);
		}

		void StartAttack()
		{
			m_State = Attack;
			m_AttackProgress = 0.0f; // Reset attack progress
			m_OutOnTrigger(1.0f);
		}

		void ProcessAttack()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_AttackProgress += m_AttackRate;
			m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
			f32 curvedProgress = glm::pow(m_AttackProgress, m_AttackCurve);
			
			// Interpolate using curved progress
			m_Value = curvedProgress; // Attack goes from 0 to 1

			// Check if attack is complete
			if (m_AttackProgress >= 1.0f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_DecayProgress = 0.0f; // Reset decay progress
			}
		}

		void ProcessDecay()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_DecayProgress += m_DecayRate;
			m_DecayProgress = glm::clamp(m_DecayProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
			f32 curvedProgress = glm::pow(m_DecayProgress, m_DecayCurve);
			
			// Interpolate using curved progress (decay goes from 1 to 0)
			m_Value = 1.0f - curvedProgress;

			// Check if decay is complete
			if (m_DecayProgress >= 1.0f)
			{
				m_Value = 0.0f;
				m_State = Idle;
				m_OutOnComplete(1.0f);

				// Handle looping
				if (*m_InLooping)
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
			m_State = Idle;
		}

		void Process() final
		{
			OLO_PROFILE_FUNCTION();
			
			// Check for parameter changes and recalculate rates if needed
			if (*m_InAttackTime != m_CachedAttackTime ||
				*m_InDecayTime != m_CachedDecayTime ||
				*m_InReleaseTime != m_CachedReleaseTime ||
				*m_InAttackCurve != m_CachedAttackCurve ||
				*m_InDecayCurve != m_CachedDecayCurve ||
				*m_InReleaseCurve != m_CachedReleaseCurve ||
				m_SampleRate != m_CachedSampleRate)
			{
				RecalculateRates();
				m_CachedAttackTime = *m_InAttackTime;
				m_CachedDecayTime = *m_InDecayTime;
				m_CachedReleaseTime = *m_InReleaseTime;
				m_CachedAttackCurve = *m_InAttackCurve;
				m_CachedDecayCurve = *m_InDecayCurve;
				m_CachedReleaseCurve = *m_InReleaseCurve;
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
				m_Value = *m_InSustainLevel;
				break;

			case Release:
				ProcessRelease();
				break;
			}

			m_OutOutEnvelope = m_Value;
		}

		// Input parameters
		f32* m_InAttackTime = nullptr;		// Attack time in seconds
		f32* m_InDecayTime = nullptr;		// Decay time in seconds
		f32* m_InSustainLevel = nullptr;	// Sustain level (0.0 to 1.0)
		f32* m_InReleaseTime = nullptr;	// Release time in seconds
		f32* m_InAttackCurve = nullptr;	// Attack curve shaping
		f32* m_InDecayCurve = nullptr;		// Decay curve shaping
		f32* m_InReleaseCurve = nullptr;	// Release curve shaping

		// Outputs
		f32 m_OutOutEnvelope{ 0.0f };

		// Output events
		OutputEvent m_OutOnTrigger{ *this };
		OutputEvent m_OutOnRelease{ *this };
		OutputEvent m_OutOnComplete{ *this };

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
		f32 m_Value{ 0.0f };
		f32 m_SustainStartValue{ 0.0f }; // Value when release started

		// Pre-calculated rates
		f32 m_AttackRate{ 0.001f };
		f32 m_DecayRate{ 0.001f };
		f32 m_ReleaseRate{ 0.001f };
		f32 m_AttackCurve{ 1.0f };
		f32 m_DecayCurve{ 1.0f };
		f32 m_ReleaseCurve{ 1.0f };

		// Normalized progress tracking (0..1)
		f32 m_AttackProgress{ 0.0f };
		f32 m_DecayProgress{ 0.0f };
		f32 m_ReleaseProgress{ 0.0f };

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
			m_AttackCurve = glm::max(0.1f, *m_InAttackCurve);
			m_DecayCurve = glm::max(0.1f, *m_InDecayCurve);
			m_ReleaseCurve = glm::max(0.1f, *m_InReleaseCurve);

			// Calculate per-sample progress increments (1/durationInSamples)
			if (*m_InAttackTime <= 0.0f)
				m_AttackRate = 1.0f; // Immediate (complete in one sample)
			else
				m_AttackRate = 1.0f / (*m_InAttackTime * m_SampleRate);

			if (*m_InDecayTime <= 0.0f)
				m_DecayRate = 1.0f; // Immediate
			else
				m_DecayRate = 1.0f / (*m_InDecayTime * m_SampleRate);

			if (*m_InReleaseTime <= 0.0f)
				m_ReleaseRate = 1.0f; // Immediate
			else
				m_ReleaseRate = 1.0f / (*m_InReleaseTime * m_SampleRate);
		}

		void StartAttack()
		{
			m_State = Attack;
			m_AttackProgress = 0.0f; // Reset attack progress
			m_OutOnTrigger(1.0f);
		}

		void StartRelease()
		{
			if (m_State != Idle && m_State != Release)
			{
				m_State = Release;
				m_SustainStartValue = m_Value;
				m_ReleaseProgress = 0.0f; // Reset release progress
				m_OutOnRelease(1.0f);
			}
		}

		void ProcessAttack()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_AttackProgress += m_AttackRate;
			m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
			f32 curvedProgress = glm::pow(m_AttackProgress, m_AttackCurve);
			
			// Interpolate using curved progress (attack goes from 0 to 1)
			m_Value = curvedProgress;

			// Check if attack is complete
			if (m_AttackProgress >= 1.0f)
			{
				m_Value = 1.0f;
				m_State = Decay;
				m_DecayProgress = 0.0f; // Reset decay progress
			}
		}

		void ProcessDecay()
		{
			// Increment normalized progress using the solved rate (preserves timing)
			m_DecayProgress += m_DecayRate;
			m_DecayProgress = glm::clamp(m_DecayProgress, 0.0f, 1.0f);
			
			// Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
			f32 curvedProgress = glm::pow(m_DecayProgress, m_DecayCurve);
			
			// Interpolate using curved progress (decay from 1.0 to sustain level)
			f32 sustainLevel = glm::clamp(*m_InSustainLevel, 0.0f, 1.0f);
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
			
			// Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
			f32 curvedProgress = glm::pow(m_ReleaseProgress, m_ReleaseCurve);
			
			// Interpolate using curved progress (release from sustain level to 0)
			m_Value = m_SustainStartValue * (1.0f - curvedProgress);

			// Check if release is complete
			if (m_ReleaseProgress >= 1.0f)
			{
				m_Value = 0.0f;
				m_State = Idle;
				m_OutOnComplete(1.0f);
			}
		}
	};
}

#undef DECLARE_ID