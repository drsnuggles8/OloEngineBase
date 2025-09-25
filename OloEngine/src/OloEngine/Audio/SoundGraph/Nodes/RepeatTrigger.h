#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// RepeatTrigger - Generates periodic trigger events
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class RepeatTrigger : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_PeriodView;
		InputView<f32> m_StartView;
		InputView<f32> m_StopView;
		
		// Output streams
		OutputView<f32> m_IsPlayingView;
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentPeriod = 1.0f;
		f32 m_CurrentStart = 0.0f;
		f32 m_CurrentStop = 0.0f;
		f32 m_CurrentIsPlaying = 0.0f;
		f32 m_CurrentOutput = 0.0f;

		//======================================================================
		// Trigger Logic State
		//======================================================================
		
		bool m_Playing = false;
		f32 m_Counter = 0.0f;
		f64 m_SampleRate = 44100.0;
		
		// Previous sample values for edge detection
		f32 m_PreviousStart = 0.0f;
		f32 m_PreviousStop = 0.0f;
		
		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;

	public:
		RepeatTrigger()
			: m_PeriodView([this](f32 value) { m_CurrentPeriod = value; }),
			  m_StartView([this](f32 value) { m_CurrentStart = value; }),
			  m_StopView([this](f32 value) { m_CurrentStop = value; }),
			  m_IsPlayingView([this](f32 value) { m_CurrentIsPlaying = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_PeriodView.Initialize(maxBufferSize);
			m_StartView.Initialize(maxBufferSize);
			m_StopView.Initialize(maxBufferSize);
			m_IsPlayingView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize sample rate and state
			m_SampleRate = sampleRate;
			m_Playing = false;
			m_Counter = 0.0f;
			m_PreviousStart = 0.0f;
			m_PreviousStop = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_PeriodView.UpdateFromConnections(inputs, numSamples);
			m_StartView.UpdateFromConnections(inputs, numSamples);
			m_StopView.UpdateFromConnections(inputs, numSamples);
			
			// Calculate frame time for timing calculations
			f32 frameTime = 1.0f / static_cast<f32>(m_SampleRate);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 period = m_PeriodView.GetValue(sample);
				f32 startValue = m_StartView.GetValue(sample);
				f32 stopValue = m_StopView.GetValue(sample);
				
				// Update internal state
				m_CurrentPeriod = period;
				m_CurrentStart = startValue;
				m_CurrentStop = stopValue;
				
				// Detect edges (rising edge detection)
				bool startEdge = startValue > TRIGGER_THRESHOLD && m_PreviousStart <= TRIGGER_THRESHOLD;
				bool stopEdge = stopValue > TRIGGER_THRESHOLD && m_PreviousStop <= TRIGGER_THRESHOLD;
				
				f32 outputTrigger = 0.0f;
				
				// Handle stop first (takes priority)
				if (stopEdge)
				{
					m_Playing = false;
					m_Counter = 0.0f;
				}
				
				// Handle start trigger
				if (startEdge)
				{
					m_Playing = true;
					m_Counter = 0.0f;
					outputTrigger = 1.0f; // Trigger immediately on start
				}
				
				// Generate periodic triggers if playing
				if (m_Playing)
				{
					m_Counter += frameTime;
					
					if (period > 0.0f && m_Counter >= period)
					{
						outputTrigger = 1.0f;
						m_Counter = 0.0f; // Reset counter after trigger
					}
				}
				
				// Update output values
				m_CurrentIsPlaying = m_Playing ? 1.0f : 0.0f;
				m_CurrentOutput = outputTrigger;
				
				m_IsPlayingView.SetValue(sample, m_CurrentIsPlaying);
				m_OutputView.SetValue(sample, outputTrigger);
				
				// Update previous values for edge detection
				m_PreviousStart = startValue;
				m_PreviousStop = stopValue;
			}
			
			// Update output streams
			m_IsPlayingView.UpdateOutputConnections(outputs, numSamples);
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("RepeatTrigger");
		}

		const char* GetDisplayName() const override
		{
			return "Repeat Trigger";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Period")) m_CurrentPeriod = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Start")) m_CurrentStart = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Stop")) m_CurrentStop = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("IsPlaying")) m_CurrentIsPlaying = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Period")) return static_cast<T>(m_CurrentPeriod);
			else if (id == OLO_IDENTIFIER("Start")) return static_cast<T>(m_CurrentStart);
			else if (id == OLO_IDENTIFIER("Stop")) return static_cast<T>(m_CurrentStop);
			else if (id == OLO_IDENTIFIER("IsPlaying")) return static_cast<T>(m_CurrentIsPlaying);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentOutput);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Check if the trigger is currently playing
		bool IsPlaying() const
		{
			return m_Playing;
		}

		/// Get the current counter value
		f32 GetCounter() const
		{
			return m_Counter;
		}

		/// Get the progress as percentage (0.0 to 1.0)
		f32 GetProgress() const
		{
			if (!m_Playing || m_CurrentPeriod <= 0.0f) return 0.0f;
			return std::clamp(m_Counter / m_CurrentPeriod, 0.0f, 1.0f);
		}

		/// Manually start the trigger
		void ManualStart()
		{
			m_Playing = true;
			m_Counter = 0.0f;
			m_CurrentIsPlaying = 1.0f;
		}

		/// Manually stop the trigger
		void ManualStop()
		{
			m_Playing = false;
			m_Counter = 0.0f;
			m_CurrentIsPlaying = 0.0f;
		}
	};

} // namespace OloEngine::Audio::SoundGraph