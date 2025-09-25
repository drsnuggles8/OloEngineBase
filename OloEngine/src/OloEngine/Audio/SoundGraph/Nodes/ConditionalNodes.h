#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// IfElseNode - Conditional node that outputs one of two values based on condition
	template<typename T>
	class IfElseNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_ConditionView;
		ValueView<T> m_TrueValueView;
		ValueView<T> m_FalseValueView;
		ValueView<T> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		f32 m_CurrentCondition = 0.0f;
		T m_CurrentTrueValue = T{};
		T m_CurrentFalseValue = T{};

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit IfElseNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_ConditionView("Condition", 0.0f)
			, m_TrueValueView("True Value", T{})
			, m_FalseValueView("False Value", T{})
			, m_OutputView("Output", T{})
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("Condition", [this](const f32& value) { m_CurrentCondition = value; });
			RegisterInputEvent<T>("True Value", [this](const T& value) { m_CurrentTrueValue = value; });
			RegisterInputEvent<T>("False Value", [this](const T& value) { m_CurrentFalseValue = value; });
			
			RegisterOutputEvent<T>("Output");
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_ConditionView.UpdateFromConnections(inputs, numSamples);
			m_TrueValueView.UpdateFromConnections(inputs, numSamples);
			m_FalseValueView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 condition = m_ConditionView.GetValue(sample);
				T trueValue = m_TrueValueView.GetValue(sample);
				T falseValue = m_FalseValueView.GetValue(sample);
				
				// Update internal state if changed
				if (condition != m_CurrentCondition) m_CurrentCondition = condition;
				if (trueValue != m_CurrentTrueValue) m_CurrentTrueValue = trueValue;
				if (falseValue != m_CurrentFalseValue) m_CurrentFalseValue = falseValue;
				
				// Perform conditional selection
				T result = (condition > 0.5f) ? trueValue : falseValue;
				
				// Set output value
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_ConditionView.Initialize(maxBufferSize);
			m_TrueValueView.Initialize(maxBufferSize);
			m_FalseValueView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("IfElseNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("IfElseNodeI32");
			else
				return OLO_IDENTIFIER("IfElseNode");
		}

		const char* GetDisplayName() const override
		{
			return "If Else";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		f32 GetCondition() const { return m_CurrentCondition; }
		void SetCondition(const f32& value) { m_CurrentCondition = value; }
		
		T GetTrueValue() const { return m_CurrentTrueValue; }
		void SetTrueValue(const T& value) { m_CurrentTrueValue = value; }
		
		T GetFalseValue() const { return m_CurrentFalseValue; }
		void SetFalseValue(const T& value) { m_CurrentFalseValue = value; }
		
		T GetOutput() const 
		{
			return (m_CurrentCondition > 0.5f) ? m_CurrentTrueValue : m_CurrentFalseValue;
		}
	};
	};

	//==============================================================================
	/// BoolToFloatNode - Converts boolean values to float (0.0 or 1.0)
	class BoolToFloatNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<f32> m_InputView;
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentOutput = 0.0f;

	public:
		BoolToFloatNode()
			: m_InputView([this](f32 value) { m_CurrentInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current value from stream
				f32 inputValue = m_InputView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				
				// Convert boolean to float (threshold at 0.5)
				f32 result = (inputValue > 0.5f) ? 1.0f : 0.0f;
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("BoolToFloatNode");
		}

		const char* GetDisplayName() const override
		{
			return "Bool To Float";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentOutput);
			return T{};
		}
	};

	//==============================================================================
	/// IntToFloatNode - Converts integer values to float
	class IntToFloatNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<i32> m_InputView;
		OutputView<f32> m_OutputView;
		
		// Current parameter values for legacy API compatibility
		i32 m_CurrentInput = 0;
		f32 m_CurrentOutput = 0.0f;

	public:
		IntToFloatNode()
			: m_InputView([this](i32 value) { m_CurrentInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current value from stream
				i32 inputValue = m_InputView.GetValue(sample);
				
				// Update internal state
				m_CurrentInput = inputValue;
				
				// Convert integer to float
				f32 result = static_cast<f32>(inputValue);
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("IntToFloatNode");
		}

		const char* GetDisplayName() const override
		{
			return "Int To Float";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Input")) m_CurrentInput = static_cast<i32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Input")) return static_cast<T>(m_CurrentInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentOutput);
			return T{};
		}
	};

	// Type aliases
	using IfElseNodeF32 = IfElseNode<f32>;
	using IfElseNodeI32 = IfElseNode<i32>;
}