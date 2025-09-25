#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// DelayedTrigger - Delays trigger events by a configurable amount
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class DelayedTrigger : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_DelayTimeView;
		InputView<f32> m_TriggerView;
		InputView<f32> m_ResetView;
		
		// Output streams
		OutputView<f32> m_DelayedOutView;
		OutputView<f32> m_PassthroughOutView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentDelayTime = 0.5f;
		f32 m_CurrentTrigger = 0.0f;
		f32 m_CurrentReset = 0.0f;
		f32 m_DelayedOutput = 0.0f;
		f32 m_PassthroughOutput = 0.0f;

		//======================================================================
		// Delay Logic State
		//======================================================================
		
		bool m_WaitingToTrigger = false;
		f32 m_DelayCounter = 0.0f;
		f64 m_SampleRate = 44100.0;
		
		// Previous sample values for edge detection
		f32 m_PreviousTrigger = 0.0f;
		f32 m_PreviousReset = 0.0f;

		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;

	public:
		DelayedTrigger()
			: m_DelayTimeView([this](f32 value) { m_CurrentDelayTime = value; }),
			  m_TriggerView([this](f32 value) { m_CurrentTrigger = value; }),
			  m_ResetView([this](f32 value) { m_CurrentReset = value; }),
			  m_DelayedOutView([this](f32 value) { m_DelayedOutput = value; }),
			  m_PassthroughOutView([this](f32 value) { m_PassthroughOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_DelayTimeView.Initialize(maxBufferSize);
			m_TriggerView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_DelayedOutView.Initialize(maxBufferSize);
			m_PassthroughOutView.Initialize(maxBufferSize);
			
			// Initialize sample rate and state
			m_SampleRate = sampleRate;
			m_WaitingToTrigger = false;
			m_DelayCounter = 0.0f;
			m_PreviousTrigger = 0.0f;
			m_PreviousReset = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_DelayTimeView.UpdateFromConnections(inputs, numSamples);
			m_TriggerView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			
			// Initialize output streams to inactive state
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				m_DelayedOutView.SetValue(sample, 0.0f);
				m_PassthroughOutView.SetValue(sample, 0.0f);
			}
			
			// Calculate frame time for delay calculations
			f32 frameTime = 1.0f / static_cast<f32>(m_SampleRate);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 delayTime = m_DelayTimeView.GetValue(sample);
				f32 triggerValue = m_TriggerView.GetValue(sample);
				f32 resetValue = m_ResetView.GetValue(sample);
				
				// Update internal state
				m_CurrentDelayTime = delayTime;
				m_CurrentTrigger = triggerValue;
				m_CurrentReset = resetValue;
				
				// Detect trigger edge (rising edge detection)
				bool triggerEdge = triggerValue > TRIGGER_THRESHOLD && m_PreviousTrigger <= TRIGGER_THRESHOLD;
				bool resetEdge = resetValue > TRIGGER_THRESHOLD && m_PreviousReset <= TRIGGER_THRESHOLD;
				
				// Handle reset first (takes priority)
				if (resetEdge)
				{
					CancelDelay();
				}
				
				// Handle trigger
				if (triggerEdge)
				{
					StartDelay(sample);
				}
				
				// Update delay countdown if waiting
				if (m_WaitingToTrigger)
				{
					m_DelayCounter += frameTime;
					
					// Check if delay time has elapsed
					if (m_DelayCounter >= delayTime)
					{
						// Fire delayed trigger
						m_DelayedOutView.SetValue(sample, 1.0f);
						m_DelayedOutput = 1.0f;
						
						// Reset delay state
						m_WaitingToTrigger = false;
						m_DelayCounter = 0.0f;
					}
				}
				
				// Update previous values for edge detection
				m_PreviousTrigger = triggerValue;
				m_PreviousReset = resetValue;
			}
			
			// Update output streams
			m_DelayedOutView.UpdateOutputConnections(outputs, numSamples);
			m_PassthroughOutView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("DelayedTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "Delayed Trigger";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("DelayTime")) m_CurrentDelayTime = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Trigger")) m_CurrentTrigger = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Reset")) m_CurrentReset = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("DelayTime")) return static_cast<T>(m_CurrentDelayTime);
			else if (id == OLO_IDENTIFIER("Trigger")) return static_cast<T>(m_CurrentTrigger);
			else if (id == OLO_IDENTIFIER("Reset")) return static_cast<T>(m_CurrentReset);
			else if (id == OLO_IDENTIFIER("DelayedOut")) return static_cast<T>(m_DelayedOutput);
			else if (id == OLO_IDENTIFIER("PassthroughOut")) return static_cast<T>(m_PassthroughOutput);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Check if the delay trigger is currently active
		bool IsWaitingToTrigger() const
		{
			return m_WaitingToTrigger;
		}

		/// Get the current delay counter (0.0 to delayTime)
		f32 GetDelayCounter() const
		{
			return m_DelayCounter;
		}

		/// Get the delay progress as percentage (0.0 to 1.0)
		f32 GetDelayProgress() const
		{
			if (!m_WaitingToTrigger || m_CurrentDelayTime <= 0.0f) return 0.0f;
			return std::clamp(m_DelayCounter / m_CurrentDelayTime, 0.0f, 1.0f);
		}

		/// Manually trigger the delay
		void ManualTrigger()
		{
			StartDelay(0);
		}

		/// Manually cancel the delay
		void ManualCancel()
		{
			CancelDelay();
		}

	private:
		void StartDelay(u32 sample)
		{
			// Fire passthrough immediately on the sample where trigger occurs
			m_PassthroughOutView.SetValue(sample, 1.0f);
			m_PassthroughOutput = 1.0f;
			
			// Start delay countdown
			m_WaitingToTrigger = true;
			m_DelayCounter = 0.0f;
		}

		void CancelDelay()
		{
			m_WaitingToTrigger = false;
			m_DelayCounter = 0.0f;
		}
	};

} // namespace OloEngine::Audio::SoundGraph