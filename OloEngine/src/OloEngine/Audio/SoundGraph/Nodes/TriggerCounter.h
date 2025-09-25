#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// TriggerCounter - Counts trigger events and generates stepped values
	/// Generates incrementing count and calculated values based on start value and step size
	class TriggerCounter : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_StartValueView;
		ValueView<f32> m_StepSizeView;
		ValueView<f32> m_ResetCountView;
		ValueView<f32> m_TriggerView;
		ValueView<f32> m_ResetView;
		ValueView<i32> m_CountOutView;
		ValueView<f32> m_ValueOutView;

		//======================================================================
		// Current Parameter Values and Internal State
		//======================================================================
		
		f32 m_CurrentStartValue = 0.0f;
		f32 m_CurrentStepSize = 1.0f;
		f32 m_CurrentResetCount = 8.0f;
		f32 m_CurrentTrigger = 0.0f;
		f32 m_CurrentReset = 0.0f;
		
		i32 m_Count = 0;
		f32 m_CurrentValue = 0.0f;
		
		bool m_PrevTriggerState = false;
		bool m_PrevResetState = false;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit TriggerCounter(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_StartValueView("StartValue", 0.0f)
			, m_StepSizeView("StepSize", 1.0f)
			, m_ResetCountView("ResetCount", 8.0f)
			, m_TriggerView("Trigger", 0.0f)
			, m_ResetView("Reset", 0.0f)
			, m_CountOutView("CountOut", 0)
			, m_ValueOutView("ValueOut", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("StartValue", [this](f32 value) { 
				m_CurrentStartValue = value; 
				// Update current value when start value changes at count 0
				if (m_Count == 0) {
					m_CurrentValue = value;
				}
			});
			RegisterInputEvent<f32>("StepSize", [this](f32 value) { m_CurrentStepSize = value; });
			RegisterInputEvent<f32>("ResetCount", [this](f32 value) { m_CurrentResetCount = value; });
			RegisterInputEvent<f32>("Trigger", [this](f32 value) { m_CurrentTrigger = value; });
			RegisterInputEvent<f32>("Reset", [this](f32 value) { m_CurrentReset = value; });
			
			RegisterOutputEvent<i32>("CountOut");
			RegisterOutputEvent<f32>("ValueOut");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_StartValueView.Initialize(maxBufferSize);
			m_StepSizeView.Initialize(maxBufferSize);
			m_ResetCountView.Initialize(maxBufferSize);
			m_TriggerView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_CountOutView.Initialize(maxBufferSize);
			m_ValueOutView.Initialize(maxBufferSize);
			
			// Reset internal state
			ResetCounter();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_StartValueView.UpdateFromConnections(inputs, numSamples);
			m_StepSizeView.UpdateFromConnections(inputs, numSamples);
			m_ResetCountView.UpdateFromConnections(inputs, numSamples);
			m_TriggerView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 startValue = m_StartValueView.GetValue(sample);
				f32 stepSize = m_StepSizeView.GetValue(sample);
				f32 resetCount = m_ResetCountView.GetValue(sample);
				f32 trigger = m_TriggerView.GetValue(sample);
				f32 reset = m_ResetView.GetValue(sample);
				
				// Update internal state
				if (startValue != m_CurrentStartValue) {
					m_CurrentStartValue = startValue;
					if (m_Count == 0) {
						m_CurrentValue = startValue;
					}
				}
				m_CurrentStepSize = stepSize;
				m_CurrentResetCount = resetCount;
				m_CurrentTrigger = trigger;
				m_CurrentReset = reset;
				
				// Check for reset (positive edge)
				bool resetState = reset > 0.5f;
				if (resetState && !m_PrevResetState)
				{
					ResetCounter();
				}
				m_PrevResetState = resetState;
				
				// Check for trigger (positive edge)
				bool triggerState = trigger > 0.5f;
				if (triggerState && !m_PrevTriggerState)
				{
					IncrementCounter();
				}
				m_PrevTriggerState = triggerState;
				
				// Set output values
				m_CountOutView.SetValue(sample, m_Count);
				m_ValueOutView.SetValue(sample, m_CurrentValue);
			}
			
			// Update output streams
			m_CountOutView.UpdateOutputConnections(outputs, numSamples);
			m_ValueOutView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetStartValue(f32 value) { TriggerInputEvent<f32>("StartValue", value); }
		void SetStepSize(f32 value) { TriggerInputEvent<f32>("StepSize", value); }
		void SetResetCount(f32 value) { TriggerInputEvent<f32>("ResetCount", value); }
		void SetTrigger(f32 value) { TriggerInputEvent<f32>("Trigger", value); }
		void SetReset(f32 value) { TriggerInputEvent<f32>("Reset", value); }
		
		i32 GetCount() const { return m_Count; }
		f32 GetValue() const { return m_CurrentValue; }
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<f32>& GetStartValueView() { return m_StartValueView; }
		ValueView<f32>& GetStepSizeView() { return m_StepSizeView; }
		ValueView<f32>& GetResetCountView() { return m_ResetCountView; }
		ValueView<f32>& GetTriggerView() { return m_TriggerView; }
		ValueView<f32>& GetResetView() { return m_ResetView; }
		ValueView<i32>& GetCountOutView() { return m_CountOutView; }
		ValueView<f32>& GetValueOutView() { return m_ValueOutView; }

		const ValueView<f32>& GetStartValueView() const { return m_StartValueView; }
		const ValueView<f32>& GetStepSizeView() const { return m_StepSizeView; }
		const ValueView<f32>& GetResetCountView() const { return m_ResetCountView; }
		const ValueView<f32>& GetTriggerView() const { return m_TriggerView; }
		const ValueView<f32>& GetResetView() const { return m_ResetView; }
		const ValueView<i32>& GetCountOutView() const { return m_CountOutView; }
		const ValueView<f32>& GetValueOutView() const { return m_ValueOutView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "StartValue" << YAML::Value << m_CurrentStartValue;
			out << YAML::Key << "StepSize" << YAML::Value << m_CurrentStepSize;
			out << YAML::Key << "ResetCount" << YAML::Value << m_CurrentResetCount;
			out << YAML::Key << "Count" << YAML::Value << m_Count;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["StartValue"]) m_CurrentStartValue = node["StartValue"].as<f32>();
			if (node["StepSize"]) m_CurrentStepSize = node["StepSize"].as<f32>();
			if (node["ResetCount"]) m_CurrentResetCount = node["ResetCount"].as<f32>();
			if (node["Count"]) {
				m_Count = node["Count"].as<i32>();
				m_CurrentValue = m_CurrentStartValue + m_CurrentStepSize * static_cast<f32>(m_Count);
			}
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			return "TriggerCounter";
		}

	private:
		//======================================================================
		// Internal Helper Methods
		//======================================================================
		
		void IncrementCounter()
		{
			m_Count++;
			
			// Calculate new value: StartValue + StepSize * Count
			m_CurrentValue = m_CurrentStartValue + m_CurrentStepSize * static_cast<f32>(m_Count);
			
			// Check for auto-reset
			if (m_CurrentResetCount > 0.5f && static_cast<f32>(m_Count) >= m_CurrentResetCount)
			{
				ResetCounter();
			}
		}
		
		void ResetCounter()
		{
			m_Count = 0;
			m_CurrentValue = m_CurrentStartValue;
		}
	};

} // namespace OloEngine::Audio::SoundGraph