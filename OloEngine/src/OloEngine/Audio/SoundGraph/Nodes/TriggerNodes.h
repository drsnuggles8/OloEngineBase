#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include <cmath>

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
	//==========================================================================
	/// RepeatTrigger - Generates periodic trigger events
	//==========================================================================
	struct RepeatTrigger : public NodeProcessor
	{
		struct IDs
		{
			DECLARE_ID(Start);
			DECLARE_ID(Stop);
		private:
			IDs() = delete;
		};

		explicit RepeatTrigger(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			AddInEvent(IDs::Start, [this](float v) { (void)v; m_StartFlag.SetDirty(); });
			AddInEvent(IDs::Stop, [this](float v) { (void)v; m_StopFlag.SetDirty(); });
			
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
				static constexpr float kMinPeriod = 0.001f; // 1ms minimum period (1000 Hz max frequency)
				float safePeriod;
				if (!std::isfinite(*in_Period) || *in_Period < kMinPeriod)
					safePeriod = kMinPeriod;
				else
					safePeriod = *in_Period;
				
				// Handle multiple periods if frame time exceeds period, preserving overshoot
				while (m_Counter >= safePeriod)
				{
					m_Counter -= safePeriod;
					out_Trigger(1.0f);
				}
			}
		}

		//==========================================================================
		/// NodeProcessor setup
		f32* in_Period = nullptr;
		OutputEvent out_Trigger{ *this };

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
			m_Playing = true;
			m_Counter = 0.0f;
			out_Trigger(1.0f);
		}

		void StopTrigger()
		{
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
			DECLARE_ID(Trigger);
			DECLARE_ID(Reset);
		private:
			IDs() = delete;
		};

		explicit TriggerCounter(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			AddInEvent(IDs::Trigger, [this](float v) { (void)v; m_TriggerFlag.SetDirty(); });
			AddInEvent(IDs::Reset, [this](float v) { (void)v; m_ResetFlag.SetDirty(); });

			RegisterEndpoints();
		}

		void Init() final
		{
			OLO_PROFILE_FUNCTION();
			
			InitializeInputs();

			out_Count = 0;
			out_Value = (*in_StartValue);
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
		f32* in_StartValue = nullptr;
		f32* in_StepSize = nullptr;
		i32* in_ResetCount = nullptr;

		i32 out_Count = 0;
		f32 out_Value = 0.0f;

		OutputEvent out_OnTrigger{ *this };
		OutputEvent out_OnReset{ *this };

	private:
		Flag m_TriggerFlag;
		Flag m_ResetFlag;
		bool m_PendingAutoReset = false;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessTrigger()
		{
			++out_Count;
			out_Value = (*in_StepSize) * out_Count + (*in_StartValue);

			out_OnTrigger(1.0f);

			// Auto-reset if we've reached the reset count (defer to end of frame)
			if ((*in_ResetCount) > 0 && out_Count >= (*in_ResetCount))
			{
				m_PendingAutoReset = true;
			}
		}

		void ProcessReset()
		{
			out_Value = (*in_StartValue);
			out_Count = 0;
			out_OnReset(1.0f);
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
			DECLARE_ID(Trigger);
			DECLARE_ID(Reset);
		private:
			IDs() = delete;
		};

		explicit DelayedTrigger(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			AddInEvent(IDs::Trigger, [this](float v) { (void)v; m_TriggerFlag.SetDirty(); });
			AddInEvent(IDs::Reset, [this](float v) { (void)v; m_ResetFlag.SetDirty(); });

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

			if (m_Waiting && (m_Counter += m_FrameTime) >= (*in_DelayTime))
			{
				m_Waiting = false;
				m_Counter = 0.0f;
				out_DelayedTrigger(1.0f);
			}
		}

		//==========================================================================
		/// NodeProcessor setup
		f32* in_DelayTime = nullptr;
		OutputEvent out_DelayedTrigger{ *this };
		OutputEvent out_OnReset{ *this };

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
			m_Waiting = true;
			m_Counter = 0.0f;
		}

		void ProcessReset()
		{
			m_Waiting = false;
			m_Counter = 0.0f;
			out_OnReset(1.0f);
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID