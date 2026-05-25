#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include <cmath>

#define DECLARE_ID(name)             \
    static constexpr Identifier name \
    {                                \
        #name                        \
    }

namespace OloEngine::Audio::SoundGraph
{
    //==========================================================================
    /// RepeatTrigger - Generates periodic trigger events
    //==========================================================================
    struct RepeatTrigger : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(s_Start);
            DECLARE_ID(s_Stop);

          private:
            IDs() = delete;
        };

        explicit RepeatTrigger(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            AddInEvent(IDs::s_Start, [this](f32 v)
                       { (void)v; m_StartFlag.SetDirty(); });
            AddInEvent(IDs::s_Stop, [this](f32 v)
                       { (void)v; m_StopFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            InitializeInputs();

            m_FrameTime = 1.0f / m_SampleRate;
            m_Counter = 0.0f;
            m_Playing = false;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            // Start/Stop flags fire at most once per audio thread iteration; checking them
            // at block boundary is the same semantics as before (events are queued
            // externally and observed at most once per Process call).
            if (m_StartFlag.CheckAndResetIfDirty())
                StartTrigger();
            if (m_StopFlag.CheckAndResetIfDirty())
                StopTrigger();

            if (!m_Playing)
                return;

            // Hoist period resolution out of the inner loop — input is stable across the block.
            static constexpr f32 kMinPeriod = 0.001f;
            f32 safePeriod;
            if (!m_Period || !std::isfinite(*m_Period) || *m_Period < kMinPeriod)
                safePeriod = kMinPeriod;
            else
                safePeriod = *m_Period;

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                m_Counter += m_FrameTime;
                while (m_Counter >= safePeriod)
                {
                    m_Counter -= safePeriod;
                    m_OutTrigger(1.0f);
                }
            }
        }

        //==========================================================================
        /// NodeProcessor setup
        f32* m_Period = nullptr;
        OutputEvent m_OutTrigger{ *this };

      private:
        bool m_Playing = false;
        f32 m_Counter = 0.0f;
        f32 m_FrameTime = 0.0f;

        Flag m_StartFlag;
        Flag m_StopFlag;

        void RegisterEndpoints();
        void InitializeInputs();

        void StartTrigger()
        {
            OLO_PROFILE_FUNCTION();

            m_Playing = true;
            m_Counter = 0.0f;
            m_OutTrigger(1.0f);
        }

        void StopTrigger()
        {
            OLO_PROFILE_FUNCTION();

            m_Playing = false;
            m_Counter = 0.0f;
        }
    };

    //==========================================================================
    /// TriggerCounter - Counts trigger events and generates step values
    //==========================================================================
    struct TriggerCounter : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(s_Trigger);
            DECLARE_ID(s_Reset);

          private:
            IDs() = delete;
        };

        explicit TriggerCounter(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            AddInEvent(IDs::s_Trigger, [this](f32 v)
                       { (void)v; m_TriggerFlag.SetDirty(); });
            AddInEvent(IDs::s_Reset, [this](f32 v)
                       { (void)v; m_ResetFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            InitializeInputs();

            m_OutCount = 0;
            // Inputs may not be wired yet (editor can leave plugs unbound until the user
            // connects them); fall through with 0 in that case instead of dereferencing
            // a nullptr.
            m_OutValue = m_StartValue ? (*m_StartValue) : 0.0f;
        }

        void Process(u32 numFrames) final
        {
            (void)numFrames; // Event-driven counter; block size doesn't affect semantics.
            OLO_PROFILE_FUNCTION();

            if (m_TriggerFlag.CheckAndResetIfDirty())
                ProcessTrigger();

            if (m_ResetFlag.CheckAndResetIfDirty())
            {
                // Clear pending auto-reset before manual reset to prevent double-reset
                m_PendingAutoReset = false;
                ProcessReset();
            }

            // Handle deferred auto-reset at end of frame
            if (m_PendingAutoReset)
            {
                ProcessReset();
                m_PendingAutoReset = false;
            }
        }

        //==========================================================================
        /// NodeProcessor setup
        f32* m_StartValue = nullptr;
        f32* m_StepSize = nullptr;
        i32* m_ResetCount = nullptr;

        i32 m_OutCount = 0;
        f32 m_OutValue = 0.0f;

        OutputEvent m_OnTrigger{ *this };
        OutputEvent m_OnReset{ *this };

      private:
        Flag m_TriggerFlag;
        Flag m_ResetFlag;
        bool m_PendingAutoReset = false;

        void RegisterEndpoints();
        void InitializeInputs();

        void ProcessTrigger()
        {
            OLO_PROFILE_FUNCTION();

            // Unbound plugs (editor hasn't wired the input yet) fall back to defaults:
            // step = 1 (count-up), start = 0, reset = 0 (no auto-reset). Dereferencing
            // nullptr here used to crash the editor as soon as you dropped a
            // TriggerCounter and fired its input event without binding the inputs.
            const f32 step = m_StepSize ? *m_StepSize : 1.0f;
            const f32 start = m_StartValue ? *m_StartValue : 0.0f;
            const i32 resetCount = m_ResetCount ? *m_ResetCount : 0;

            ++m_OutCount;
            m_OutValue = step * m_OutCount + start;

            m_OnTrigger(1.0f);

            // Auto-reset if we've reached the reset count (defer to end of frame)
            if (resetCount > 0 && m_OutCount >= resetCount)
            {
                m_PendingAutoReset = true;
            }
        }

        void ProcessReset()
        {
            OLO_PROFILE_FUNCTION();

            m_OutValue = m_StartValue ? (*m_StartValue) : 0.0f;
            m_OutCount = 0;
            m_OnReset(1.0f);
            m_PendingAutoReset = false;
        }
    };

    //==========================================================================
    /// DelayedTrigger - Delays trigger events by a specified time
    //==========================================================================
    struct DelayedTrigger : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(s_Trigger);
            DECLARE_ID(s_Reset);

          private:
            IDs() = delete;
        };

        explicit DelayedTrigger(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            AddInEvent(IDs::s_Trigger, [this](f32 v)
                       { (void)v; m_TriggerFlag.SetDirty(); });
            AddInEvent(IDs::s_Reset, [this](f32 v)
                       { (void)v; m_ResetFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            InitializeInputs();

            m_FrameTime = 1.0f / m_SampleRate;
            m_Counter = 0.0f;
            m_Waiting = false;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            if (m_TriggerFlag.CheckAndResetIfDirty())
                StartDelay();

            if (m_ResetFlag.CheckAndResetIfDirty())
                ProcessReset();

            // Unbound delay → behave like zero-delay passthrough. Guards against the
            // editor-not-yet-wired case the rest of these nodes already handle.
            const f32 delay = m_DelayTime ? *m_DelayTime : 0.0f;

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                if (m_Waiting && (m_Counter += m_FrameTime) >= delay)
                {
                    m_Waiting = false;
                    m_Counter = 0.0f;
                    m_OutDelayedTrigger(1.0f);
                }
            }
        }

        //==========================================================================
        /// NodeProcessor setup
        f32* m_DelayTime = nullptr;
        OutputEvent m_OutDelayedTrigger{ *this };
        OutputEvent m_OnReset{ *this };

      private:
        bool m_Waiting = false;
        f32 m_Counter = 0.0f;
        f32 m_FrameTime = 0.0f;

        Flag m_TriggerFlag;
        Flag m_ResetFlag;

        void RegisterEndpoints();
        void InitializeInputs();

        void StartDelay()
        {
            OLO_PROFILE_FUNCTION();

            m_Waiting = true;
            m_Counter = 0.0f;
        }

        void ProcessReset()
        {
            OLO_PROFILE_FUNCTION();

            m_Waiting = false;
            m_Counter = 0.0f;
            m_OnReset(1.0f);
        }
    };

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
