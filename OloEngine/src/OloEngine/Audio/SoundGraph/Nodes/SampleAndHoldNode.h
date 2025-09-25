#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// SampleAndHoldNode - Samples and holds an input value when triggered
	/// Useful for creating stepped sequences or randomized control values
	class SampleAndHoldNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_InputView;
		ValueView<f32> m_TriggerView;
		ValueView<f32> m_ResetView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Internal State
		//======================================================================
		
		f32 m_HeldValue = 0.0f;
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentTrigger = 0.0f;
		f32 m_CurrentReset = 0.0f;
		
		bool m_PrevTriggerState = false;
		bool m_PrevResetState = false;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit SampleAndHoldNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputView("Input", 0.0f)
			, m_TriggerView("Trigger", 0.0f)
			, m_ResetView("Reset", 0.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Input", [this](f32 value) { m_CurrentInput = value; });
			RegisterInputEvent<f32>("Trigger", [this](f32 value) { m_CurrentTrigger = value; });
			RegisterInputEvent<f32>("Reset", [this](f32 value) { m_CurrentReset = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_TriggerView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Reset internal state
			m_HeldValue = 0.0f;
			m_PrevTriggerState = false;
			m_PrevResetState = false;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_TriggerView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 input = m_InputView.GetValue(sample);
				f32 trigger = m_TriggerView.GetValue(sample);
				f32 reset = m_ResetView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = input;
				m_CurrentTrigger = trigger;
				m_CurrentReset = reset;
				
				// Check for reset (positive edge)
				bool resetState = reset > 0.5f;
				if (resetState && !m_PrevResetState)
				{
					m_HeldValue = 0.0f;
				}
				m_PrevResetState = resetState;
				
				// Check for trigger (positive edge)
				bool triggerState = trigger > 0.5f;
				if (triggerState && !m_PrevTriggerState)
				{
					// Sample the input value
					m_HeldValue = input;
				}
				m_PrevTriggerState = triggerState;
				
				// Set output value
				m_OutputView.SetValue(sample, m_HeldValue);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetInput(f32 value) { TriggerInputEvent<f32>("Input", value); }
		void SetTrigger(f32 value) { TriggerInputEvent<f32>("Trigger", value); }
		void SetReset(f32 value) { TriggerInputEvent<f32>("Reset", value); }
		f32 GetOutput() const { return m_HeldValue; }
		
		f32 GetHeldValue() const { return m_HeldValue; }
		void SetHeldValue(f32 value) { m_HeldValue = value; }
		void ResetHold() { m_HeldValue = 0.0f; }
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<f32>& GetInputView() { return m_InputView; }
		ValueView<f32>& GetTriggerView() { return m_TriggerView; }
		ValueView<f32>& GetResetView() { return m_ResetView; }
		ValueView<f32>& GetOutputView() { return m_OutputView; }

		const ValueView<f32>& GetInputView() const { return m_InputView; }
		const ValueView<f32>& GetTriggerView() const { return m_TriggerView; }
		const ValueView<f32>& GetResetView() const { return m_ResetView; }
		const ValueView<f32>& GetOutputView() const { return m_OutputView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "HeldValue" << YAML::Value << m_HeldValue;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["HeldValue"]) m_HeldValue = node["HeldValue"].as<f32>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			return "SampleAndHoldNode";
		}
	};

} // namespace OloEngine::Audio::SoundGraph