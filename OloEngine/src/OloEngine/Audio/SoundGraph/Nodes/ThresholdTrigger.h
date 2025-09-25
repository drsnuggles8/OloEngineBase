#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// ThresholdTrigger - Triggers when input value crosses a threshold
	/// Supports both rising and falling edge detection with hysteresis
	class ThresholdTrigger : public NodeProcessor
	{
	public:
		// Trigger modes
		enum class TriggerMode : i32
		{
			Rising = 0,   // Trigger when input rises above threshold
			Falling = 1,  // Trigger when input falls below threshold
			Both = 2      // Trigger on both rising and falling edges
		};

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_InputView;
		ValueView<f32> m_ThresholdView;
		ValueView<f32> m_HysteresisView;
		ValueView<i32> m_ModeView;
		ValueView<f32> m_ResetView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Internal State
		//======================================================================
		
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentThreshold = 0.5f;
		f32 m_CurrentHysteresis = 0.01f;
		TriggerMode m_CurrentMode = TriggerMode::Rising;
		f32 m_CurrentReset = 0.0f;
		
		bool m_LastState = false;
		bool m_PrevResetState = false;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit ThresholdTrigger(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputView("Input", 0.0f)
			, m_ThresholdView("Threshold", 0.5f)
			, m_HysteresisView("Hysteresis", 0.01f)
			, m_ModeView("Mode", static_cast<i32>(TriggerMode::Rising))
			, m_ResetView("Reset", 0.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Input", [this](f32 value) { m_CurrentInput = value; });
			RegisterInputEvent<f32>("Threshold", [this](f32 value) { m_CurrentThreshold = value; });
			RegisterInputEvent<f32>("Hysteresis", [this](f32 value) { m_CurrentHysteresis = value; });
			RegisterInputEvent<i32>("Mode", [this](i32 value) { m_CurrentMode = static_cast<TriggerMode>(value); });
			RegisterInputEvent<f32>("Reset", [this](f32 value) { m_CurrentReset = value; });
			
			RegisterOutputEvent<f32>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_ThresholdView.Initialize(maxBufferSize);
			m_HysteresisView.Initialize(maxBufferSize);
			m_ModeView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Reset internal state
			m_LastState = false;
			m_PrevResetState = false;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_ThresholdView.UpdateFromConnections(inputs, numSamples);
			m_HysteresisView.UpdateFromConnections(inputs, numSamples);
			m_ModeView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 inputValue = m_InputView.GetValue(sample);
				f32 threshold = m_ThresholdView.GetValue(sample);
				f32 hysteresis = m_HysteresisView.GetValue(sample);
				i32 modeValue = m_ModeView.GetValue(sample);
				f32 reset = m_ResetView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				m_CurrentThreshold = threshold;
				m_CurrentHysteresis = hysteresis;
				m_CurrentMode = static_cast<TriggerMode>(modeValue);
				m_CurrentReset = reset;
				
				// Check for reset (positive edge)
				bool resetState = reset > 0.5f;
				if (resetState && !m_PrevResetState)
				{
					m_LastState = false;
				}
				m_PrevResetState = resetState;
				
				// Calculate thresholds with hysteresis
				f32 halfHysteresis = hysteresis * 0.5f;
				f32 upperThreshold = threshold + halfHysteresis;
				f32 lowerThreshold = threshold - halfHysteresis;
				
				// Determine current state with hysteresis
				bool currentState;
				if (m_LastState)
				{
					// If we were above threshold, need to fall below lower threshold
					currentState = inputValue > lowerThreshold;
				}
				else
				{
					// If we were below threshold, need to rise above upper threshold
					currentState = inputValue > upperThreshold;
				}
				
				// Check for state change and trigger accordingly
				f32 outputValue = 0.0f;
				bool shouldTrigger = false;
				
				if (currentState != m_LastState)
				{
					switch (m_CurrentMode)
					{
						case TriggerMode::Rising:
							shouldTrigger = currentState && !m_LastState; // Rising edge
							break;
						case TriggerMode::Falling:
							shouldTrigger = !currentState && m_LastState; // Falling edge
							break;
						case TriggerMode::Both:
							shouldTrigger = true; // Any edge
							break;
					}
					
					if (shouldTrigger)
						outputValue = 1.0f;
				}
				
				m_LastState = currentState;
				
				// Set output value
				m_OutputView.SetValue(sample, outputValue);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetInput(f32 value) { TriggerInputEvent<f32>("Input", value); }
		void SetThreshold(f32 value) { TriggerInputEvent<f32>("Threshold", value); }
		void SetHysteresis(f32 value) { TriggerInputEvent<f32>("Hysteresis", value); }
		void SetMode(TriggerMode mode) { TriggerInputEvent<i32>("Mode", static_cast<i32>(mode)); }
		void SetReset(f32 value) { TriggerInputEvent<f32>("Reset", value); }
		
		f32 GetInput() const { return m_CurrentInput; }
		f32 GetThreshold() const { return m_CurrentThreshold; }
		f32 GetHysteresis() const { return m_CurrentHysteresis; }
		TriggerMode GetMode() const { return m_CurrentMode; }
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<f32>& GetInputView() { return m_InputView; }
		ValueView<f32>& GetThresholdView() { return m_ThresholdView; }
		ValueView<f32>& GetHysteresisView() { return m_HysteresisView; }
		ValueView<i32>& GetModeView() { return m_ModeView; }
		ValueView<f32>& GetResetView() { return m_ResetView; }
		ValueView<f32>& GetOutputView() { return m_OutputView; }

		const ValueView<f32>& GetInputView() const { return m_InputView; }
		const ValueView<f32>& GetThresholdView() const { return m_ThresholdView; }
		const ValueView<f32>& GetHysteresisView() const { return m_HysteresisView; }
		const ValueView<i32>& GetModeView() const { return m_ModeView; }
		const ValueView<f32>& GetResetView() const { return m_ResetView; }
		const ValueView<f32>& GetOutputView() const { return m_OutputView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "Input" << YAML::Value << m_CurrentInput;
			out << YAML::Key << "Threshold" << YAML::Value << m_CurrentThreshold;
			out << YAML::Key << "Hysteresis" << YAML::Value << m_CurrentHysteresis;
			out << YAML::Key << "Mode" << YAML::Value << static_cast<i32>(m_CurrentMode);
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["Input"]) m_CurrentInput = node["Input"].as<f32>();
			if (node["Threshold"]) m_CurrentThreshold = node["Threshold"].as<f32>();
			if (node["Hysteresis"]) m_CurrentHysteresis = node["Hysteresis"].as<f32>();
			if (node["Mode"]) m_CurrentMode = static_cast<TriggerMode>(node["Mode"].as<i32>());
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			return "ThresholdTrigger";
		}
	};

} // namespace OloEngine::Audio::SoundGraph