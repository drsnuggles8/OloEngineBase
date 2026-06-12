#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#define DECLARE_ID(name)             \
    static constexpr Identifier name \
    {                                \
        #name                        \
    }

namespace OloEngine::Audio::SoundGraph
{
    // Tolerance for "did the user actually change this parameter?" checks below. We
    // recompute attack/decay/release rates when any input differs from its cached value;
    // bit-exact != on f32 makes us recompute every block whenever the producer chain
    // introduces sub-ULP rounding (interpolation ramps, accumulating sums, etc.). 1e-6
    // is small enough that any human-meaningful change still flips the check.
    inline constexpr f32 kEnvelopeParamEpsilon = 1e-6f;
    inline bool EnvelopeParamChanged(f32 a, f32 b) noexcept
    {
        return std::fabs(a - b) > kEnvelopeParamEpsilon;
    }

    // Control refs can be wired to arbitrary producers, so a NaN/Inf upstream must
    // not poison the envelope's cached rates or progress state. Non-finite reads
    // fall back to the last good (cached) value, or zero if none exists yet.
    inline f32 SanitizeEnvelopeParam(f32 value, f32 fallback) noexcept
    {
        if (std::isfinite(value))
            return value;
        return std::isfinite(fallback) ? fallback : 0.0f;
    }

    //==============================================================================
    // Attack-Decay Envelope Generator
    //==============================================================================
    struct ADEnvelope : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(s_Trigger);

          private:
            IDs() = delete;
        };

        explicit ADEnvelope(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            // Input events
            AddInEvent(IDs::s_Trigger, [this](float v)
                       { (void)v; m_TriggerFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            // Sample rate is now set by NodeProcessor base class
            RecalculateRates(SanitizeEnvelopeParam(m_AttackTime.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_DecayTime.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_AttackCurve.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_DecayCurve.Get(), 0.0f));

            // Initialize state
            m_Value = 0.0f;
            m_State = Idle;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            // Parameter check + recalc: block-rate (control inputs are stable across
            // the block). Sanitize before dirty-checking so a non-finite upstream
            // value can never enter the rate math or the caches.
            const f32 attackTime = SanitizeEnvelopeParam(m_AttackTime.Get(), m_CachedAttackTime);
            const f32 decayTime = SanitizeEnvelopeParam(m_DecayTime.Get(), m_CachedDecayTime);
            const f32 attackCurve = SanitizeEnvelopeParam(m_AttackCurve.Get(), m_CachedAttackCurve);
            const f32 decayCurve = SanitizeEnvelopeParam(m_DecayCurve.Get(), m_CachedDecayCurve);
            if (EnvelopeParamChanged(attackTime, m_CachedAttackTime) ||
                EnvelopeParamChanged(decayTime, m_CachedDecayTime) ||
                EnvelopeParamChanged(attackCurve, m_CachedAttackCurve) ||
                EnvelopeParamChanged(decayCurve, m_CachedDecayCurve) ||
                EnvelopeParamChanged(m_SampleRate, m_CachedSampleRate))
            {
                RecalculateRates(attackTime, decayTime, attackCurve, decayCurve);
                m_CachedAttackTime = attackTime;
                m_CachedDecayTime = decayTime;
                m_CachedAttackCurve = attackCurve;
                m_CachedDecayCurve = decayCurve;
                m_CachedSampleRate = m_SampleRate;
            }

            // Handle trigger events: block boundary is acceptable for now (Phase 4 adds
            // sample-offset triggers when sample accuracy matters).
            if (m_TriggerFlag.IsDirty())
            {
                m_TriggerFlag.CheckAndResetIfDirty();
                StartAttack();
            }

            // State machine advances one sample per iteration.
            f32* out = m_OutEnvelope.Data();
            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                switch (m_State)
                {
                    case Idle:
                        break;
                    case Attack:
                        ProcessAttack();
                        break;
                    case Decay:
                        ProcessDecay();
                        break;
                }
                out[frame] = m_Value;
            }
        }

        // Input parameters (control-rate)
        FloatRef m_AttackTime;  // Attack time in seconds
        FloatRef m_DecayTime;   // Decay time in seconds
        FloatRef m_AttackCurve; // Attack curve shaping (1.0 = linear, >1 = convex, <1 = concave)
        FloatRef m_DecayCurve;  // Decay curve shaping
        BoolRef m_Looping;      // Enable looping (retrigger after decay)

        // Outputs (audio-rate)
        AudioBuffer m_OutEnvelope;

        // Output events
        OutputEvent m_OnTrigger{ *this };
        OutputEvent m_OnComplete{ *this };

        void RegisterEndpoints();

      private:
        enum State
        {
            Idle = 0,   // Before attack / after decay
            Attack = 1, // Rising phase
            Decay = 2,  // Falling phase
        };

        State m_State{ Idle };
        f32 m_Value{ 0.0f };

        // Pre-calculated rates and effective curves (clamped + cached values derived
        // from the input refs — kept under distinct names so they don't shadow the
        // m_AttackCurve / m_DecayCurve input refs).
        f32 m_AttackRate{ 0.001f };
        f32 m_DecayRate{ 0.001f };
        f32 m_EffectiveAttackCurve{ 1.0f };
        f32 m_EffectiveDecayCurve{ 1.0f };

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

        // Inputs must be pre-sanitized (finite) by the caller — see SanitizeEnvelopeParam.
        void RecalculateRates(f32 attackTime, f32 decayTime, f32 attackCurve, f32 decayCurve)
        {
            OLO_PROFILE_FUNCTION();
            if (m_SampleRate <= 0.0f)
                return;

            m_EffectiveAttackCurve = glm::max(0.1f, attackCurve);
            m_EffectiveDecayCurve = glm::max(0.1f, decayCurve);

            // Calculate attack rate (linear per-sample increment to reach 1.0 in specified time)
            if (attackTime <= 0.0f)
                m_AttackRate = 1.0f; // Immediate
            else
                m_AttackRate = 1.0f / (attackTime * m_SampleRate);

            // Calculate decay rate (linear per-sample increment to reach 1.0 in specified time)
            if (decayTime <= 0.0f)
                m_DecayRate = 1.0f; // Immediate
            else
                m_DecayRate = 1.0f / (decayTime * m_SampleRate);
        }

        void StartAttack()
        {
            m_State = Attack;
            m_AttackProgress = 0.0f; // Reset attack progress
            m_OnTrigger(1.0f);
        }

        void ProcessAttack()
        {
            // Increment normalized progress using the solved rate (preserves timing)
            m_AttackProgress += m_AttackRate;
            m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);

            // Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
            f32 curvedProgress = glm::pow(m_AttackProgress, m_EffectiveAttackCurve);

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
            f32 curvedProgress = glm::pow(m_DecayProgress, m_EffectiveDecayCurve);

            // Interpolate using curved progress (decay goes from 1 to 0)
            m_Value = 1.0f - curvedProgress;

            // Check if decay is complete
            if (m_DecayProgress >= 1.0f)
            {
                m_Value = 0.0f;
                m_State = Idle;
                m_OnComplete(1.0f);

                // Handle looping
                if (m_Looping.Get())
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
            DECLARE_ID(s_Trigger);
            DECLARE_ID(s_Release);

          private:
            IDs() = delete;
        };

        explicit ADSREnvelope(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            // Input events
            AddInEvent(IDs::s_Trigger, [this](float v)
                       { (void)v; m_TriggerFlag.SetDirty(); });
            AddInEvent(IDs::s_Release, [this](float v)
                       { (void)v; m_ReleaseFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            // Sample rate is now set by NodeProcessor base class
            RecalculateRates(SanitizeEnvelopeParam(m_AttackTime.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_DecayTime.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_ReleaseTime.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_AttackCurve.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_DecayCurve.Get(), 0.0f),
                             SanitizeEnvelopeParam(m_ReleaseCurve.Get(), 0.0f));

            // Initialize state
            m_Value = 0.0f;
            m_State = Idle;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            // Parameter check + recalc: block-rate. Sanitize before dirty-checking so
            // a non-finite upstream value can never enter the rate math or the caches.
            const f32 attackTime = SanitizeEnvelopeParam(m_AttackTime.Get(), m_CachedAttackTime);
            const f32 decayTime = SanitizeEnvelopeParam(m_DecayTime.Get(), m_CachedDecayTime);
            const f32 releaseTime = SanitizeEnvelopeParam(m_ReleaseTime.Get(), m_CachedReleaseTime);
            const f32 attackCurve = SanitizeEnvelopeParam(m_AttackCurve.Get(), m_CachedAttackCurve);
            const f32 decayCurve = SanitizeEnvelopeParam(m_DecayCurve.Get(), m_CachedDecayCurve);
            const f32 releaseCurve = SanitizeEnvelopeParam(m_ReleaseCurve.Get(), m_CachedReleaseCurve);
            if (EnvelopeParamChanged(attackTime, m_CachedAttackTime) ||
                EnvelopeParamChanged(decayTime, m_CachedDecayTime) ||
                EnvelopeParamChanged(releaseTime, m_CachedReleaseTime) ||
                EnvelopeParamChanged(attackCurve, m_CachedAttackCurve) ||
                EnvelopeParamChanged(decayCurve, m_CachedDecayCurve) ||
                EnvelopeParamChanged(releaseCurve, m_CachedReleaseCurve) ||
                EnvelopeParamChanged(m_SampleRate, m_CachedSampleRate))
            {
                RecalculateRates(attackTime, decayTime, releaseTime, attackCurve, decayCurve, releaseCurve);
                m_CachedAttackTime = attackTime;
                m_CachedDecayTime = decayTime;
                m_CachedReleaseTime = releaseTime;
                m_CachedAttackCurve = attackCurve;
                m_CachedDecayCurve = decayCurve;
                m_CachedReleaseCurve = releaseCurve;
                m_CachedSampleRate = m_SampleRate;
            }

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

            f32* out = m_OutEnvelope.Data();
            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                switch (m_State)
                {
                    case Idle:
                        break;
                    case Attack:
                        ProcessAttack();
                        break;
                    case Decay:
                        ProcessDecay();
                        break;
                    case Sustain:
                        // Clamp like ProcessDecay does, so an out-of-range (or
                        // non-finite) control value can't push m_Value outside [0,1]
                        // and later be captured by StartRelease().
                        m_Value = glm::clamp(SanitizeEnvelopeParam(m_SustainLevel.Get(), 0.0f), 0.0f, 1.0f);
                        break;
                    case Release:
                        ProcessRelease();
                        break;
                }
                out[frame] = m_Value;
            }
        }

        // Input parameters (control-rate)
        FloatRef m_AttackTime;   // Attack time in seconds
        FloatRef m_DecayTime;    // Decay time in seconds
        FloatRef m_SustainLevel; // Sustain level (0.0 to 1.0)
        FloatRef m_ReleaseTime;  // Release time in seconds
        FloatRef m_AttackCurve;  // Attack curve shaping
        FloatRef m_DecayCurve;   // Decay curve shaping
        FloatRef m_ReleaseCurve; // Release curve shaping

        // Outputs (audio-rate)
        AudioBuffer m_OutEnvelope;

        // Output events
        OutputEvent m_OnTrigger{ *this };
        OutputEvent m_OnRelease{ *this };
        OutputEvent m_OnComplete{ *this };

        void RegisterEndpoints();

      private:
        enum State
        {
            Idle = 0,    // Before trigger / after release
            Attack = 1,  // Rising to peak
            Decay = 2,   // Falling to sustain
            Sustain = 3, // Holding sustain level
            Release = 4, // Falling to zero
        };

        State m_State{ Idle };
        f32 m_Value{ 0.0f };
        f32 m_SustainStartValue{ 0.0f }; // Value when release started

        // Pre-calculated rates
        f32 m_AttackRate{ 0.001f };
        f32 m_DecayRate{ 0.001f };
        f32 m_ReleaseRate{ 0.001f };
        // Effective (clamped) curve values, kept distinct from the m_*Curve input refs.
        f32 m_EffectiveAttackCurve{ 1.0f };
        f32 m_EffectiveDecayCurve{ 1.0f };
        f32 m_EffectiveReleaseCurve{ 1.0f };

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

        // Inputs must be pre-sanitized (finite) by the caller — see SanitizeEnvelopeParam.
        void RecalculateRates(f32 attackTime, f32 decayTime, f32 releaseTime,
                              f32 attackCurve, f32 decayCurve, f32 releaseCurve)
        {
            OLO_PROFILE_FUNCTION();
            if (m_SampleRate <= 0.0f)
                return;
            m_EffectiveAttackCurve = glm::max(0.1f, attackCurve);
            m_EffectiveDecayCurve = glm::max(0.1f, decayCurve);
            m_EffectiveReleaseCurve = glm::max(0.1f, releaseCurve);

            // Calculate per-sample progress increments (1/durationInSamples)
            if (attackTime <= 0.0f)
                m_AttackRate = 1.0f; // Immediate (complete in one sample)
            else
                m_AttackRate = 1.0f / (attackTime * m_SampleRate);

            if (decayTime <= 0.0f)
                m_DecayRate = 1.0f; // Immediate
            else
                m_DecayRate = 1.0f / (decayTime * m_SampleRate);

            if (releaseTime <= 0.0f)
                m_ReleaseRate = 1.0f; // Immediate
            else
                m_ReleaseRate = 1.0f / (releaseTime * m_SampleRate);
        }

        void StartAttack()
        {
            m_State = Attack;
            m_AttackProgress = 0.0f; // Reset attack progress
            m_OnTrigger(1.0f);
        }

        void StartRelease()
        {
            if (m_State != Idle && m_State != Release)
            {
                m_State = Release;
                m_SustainStartValue = m_Value;
                m_ReleaseProgress = 0.0f; // Reset release progress
                m_OnRelease(1.0f);
            }
        }

        void ProcessAttack()
        {
            // Increment normalized progress using the solved rate (preserves timing)
            m_AttackProgress += m_AttackRate;
            m_AttackProgress = glm::clamp(m_AttackProgress, 0.0f, 1.0f);

            // Apply curve to normalized progress (curve>1 = convex/slow-start, curve<1 = concave/fast-start)
            f32 curvedProgress = glm::pow(m_AttackProgress, m_EffectiveAttackCurve);

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
            f32 curvedProgress = glm::pow(m_DecayProgress, m_EffectiveDecayCurve);

            // Interpolate using curved progress (decay from 1.0 to sustain level)
            f32 sustainLevel = glm::clamp(SanitizeEnvelopeParam(m_SustainLevel.Get(), 0.0f), 0.0f, 1.0f);
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
            f32 curvedProgress = glm::pow(m_ReleaseProgress, m_EffectiveReleaseCurve);

            // Interpolate using curved progress (release from sustain level to 0)
            m_Value = m_SustainStartValue * (1.0f - curvedProgress);

            // Check if release is complete
            if (m_ReleaseProgress >= 1.0f)
            {
                m_Value = 0.0f;
                m_State = Idle;
                m_OnComplete(1.0f);
            }
        }
    };
} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
