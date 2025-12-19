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

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (m_StartFlag.CheckAndResetIfDirty())
                StartTrigger();
            if (m_StopFlag.CheckAndResetIfDirty())
                StopTrigger();

            if (m_Playing)
            {
                m_Counter += m_FrameTime;

                // Guard against zero/negative/non-finite period to prevent infinite loop
                static constexpr f32 kMinPeriod = 0.001f; // 1ms minimum period (1000 Hz max frequency)
                f32 safePeriod;
                if (!std::isfinite(*m_InPeriod) || *m_InPeriod < kMinPeriod)
                    safePeriod = kMinPeriod;
                else
                    safePeriod = *m_InPeriod;

                // Handle multiple periods if frame time exceeds period, preserving overshoot
                while (m_Counter >= safePeriod)
                {
                    m_Counter -= safePeriod;
                    m_OutTrigger(1.0f);
                }
            }
        }

        //==========================================================================
        /// NodeProcessor setup
        f32* m_InPeriod = nullptr;
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
            m_OutValue = (*m_InStartValue);
        }

        void Process() final
        {
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
        f32* m_InStartValue = nullptr;
        f32* m_InStepSize = nullptr;
        i32* m_InResetCount = nullptr;

        i32 m_OutCount = 0;
        f32 m_OutValue = 0.0f;

        OutputEvent m_OutOnTrigger{ *this };
        OutputEvent m_OutOnReset{ *this };

      private:
        Flag m_TriggerFlag;
        Flag m_ResetFlag;
        bool m_PendingAutoReset = false;

        void RegisterEndpoints();
        void InitializeInputs();

        void ProcessTrigger()
        {
            OLO_PROFILE_FUNCTION();

            ++m_OutCount;
            m_OutValue = (*m_InStepSize) * m_OutCount + (*m_InStartValue);

            m_OutOnTrigger(1.0f);

            // Auto-reset if we've reached the reset count (defer to end of frame)
            if ((*m_InResetCount) > 0 && m_OutCount >= (*m_InResetCount))
            {
                m_PendingAutoReset = true;
            }
        }

        void ProcessReset()
        {
            OLO_PROFILE_FUNCTION();

            m_OutValue = (*m_InStartValue);
            m_OutCount = 0;
            m_OutOnReset(1.0f);
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

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            if (m_TriggerFlag.CheckAndResetIfDirty())
                StartDelay();

            if (m_ResetFlag.CheckAndResetIfDirty())
                ProcessReset();

            if (m_Waiting && (m_Counter += m_FrameTime) >= (*m_InDelayTime))
            {
                m_Waiting = false;
                m_Counter = 0.0f;
                m_OutDelayedTrigger(1.0f);
            }
        }

        //==========================================================================
        /// NodeProcessor setup
        f32* m_InDelayTime = nullptr;
        OutputEvent m_OutDelayedTrigger{ *this };
        OutputEvent m_OutOnReset{ *this };

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
            m_OutOnReset(1.0f);
        }
    };

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
