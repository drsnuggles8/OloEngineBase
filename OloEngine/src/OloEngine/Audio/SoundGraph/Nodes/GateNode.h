#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// GateNode - Gates signal based on control input
	/// Allows signal to pass through only when gate control is above threshold
	class GateNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_InputView;
		ValueView<f32> m_GateView;
		ValueView<f32> m_ThresholdView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentGate = 0.0f;
		f32 m_CurrentThreshold = 0.5f;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit GateNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputView("Input", 0.0f)
			, m_GateView("Gate", 0.0f)
			, m_ThresholdView("Threshold", 0.5f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Input", [this](f32 value) { m_CurrentInput = value; });
			RegisterInputEvent<f32>("Gate", [this](f32 value) { m_CurrentGate = value; });
			RegisterInputEvent<f32>("Threshold", [this](f32 value) { m_CurrentThreshold = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_GateView.Initialize(maxBufferSize);
			m_ThresholdView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_GateView.UpdateFromConnections(inputs, numSamples);
			m_ThresholdView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 input = m_InputView.GetValue(sample);
				f32 gate = m_GateView.GetValue(sample);
				f32 threshold = m_ThresholdView.GetValue(sample);
				
				// Update internal state if changed
				if (input != m_CurrentInput) m_CurrentInput = input;
				if (gate != m_CurrentGate) m_CurrentGate = gate;
				if (threshold != m_CurrentThreshold) m_CurrentThreshold = threshold;
				
				// Gate logic: pass input through only when gate is above threshold
				bool gateOpen = gate > threshold;
				f32 output = gateOpen ? input : 0.0f;
				
				// Set output value
				m_OutputView.SetValue(sample, output);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetInput(f32 value) { TriggerInputEvent<f32>("Input", value); }
		void SetGate(f32 value) { TriggerInputEvent<f32>("Gate", value); }
		void SetThreshold(f32 value) { TriggerInputEvent<f32>("Threshold", value); }
		f32 GetOutput() const { return IsGateOpen() ? m_CurrentInput : 0.0f; }
		
		f32 GetThreshold() const { return m_CurrentThreshold; }
		bool IsGateOpen() const { return m_CurrentGate > m_CurrentThreshold; }
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<f32>& GetInputView() { return m_InputView; }
		ValueView<f32>& GetGateView() { return m_GateView; }
		ValueView<f32>& GetThresholdView() { return m_ThresholdView; }
		ValueView<f32>& GetOutputView() { return m_OutputView; }

		const ValueView<f32>& GetInputView() const { return m_InputView; }
		const ValueView<f32>& GetGateView() const { return m_GateView; }
		const ValueView<f32>& GetThresholdView() const { return m_ThresholdView; }
		const ValueView<f32>& GetOutputView() const { return m_OutputView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "Input" << YAML::Value << m_CurrentInput;
			out << YAML::Key << "Gate" << YAML::Value << m_CurrentGate;
			out << YAML::Key << "Threshold" << YAML::Value << m_CurrentThreshold;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["Input"]) m_CurrentInput = node["Input"].as<f32>();
			if (node["Gate"]) m_CurrentGate = node["Gate"].as<f32>();
			if (node["Threshold"]) m_CurrentThreshold = node["Threshold"].as<f32>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			return "GateNode";
		}
	};

} // namespace OloEngine::Audio::SoundGraph