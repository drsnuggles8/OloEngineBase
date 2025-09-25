#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <algorithm>
#include <type_traits>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// Min node - Returns the minimum of two values
	/// Supports both real-time audio streams and single-value processing
	template<typename T>
	class MinNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "MinNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_InputAView;
		ValueView<T> m_InputBView;
		ValueView<T> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentA = T{};
		T m_CurrentB = T{};

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit MinNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputAView("Input A", T{})
			, m_InputBView("Input B", T{})
			, m_OutputView("Output", T{})
		{
			// Create Input/Output events
			RegisterInputEvent<T>("Input A", [this](const T& value) { m_CurrentA = value; });
			RegisterInputEvent<T>("Input B", [this](const T& value) { m_CurrentB = value; });
			
			RegisterOutputEvent<T>("Output");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputAView.Initialize(maxBufferSize);
			m_InputBView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputAView.UpdateFromConnections(inputs, numSamples);
			m_InputBView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T valueA = m_InputAView.GetValue(sample);
				T valueB = m_InputBView.GetValue(sample);
				
				// Update internal state if changed
				if (valueA != m_CurrentA) m_CurrentA = valueA;
				if (valueB != m_CurrentB) m_CurrentB = valueB;
				
				// Calculate minimum
				T result = std::min(valueA, valueB);
				
				// Set output value
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetInputA(const T& value) { TriggerInputEvent<T>("Input A", value); }
		void SetInputB(const T& value) { TriggerInputEvent<T>("Input B", value); }
		T GetOutput() const { return m_CurrentA < m_CurrentB ? m_CurrentA : m_CurrentB; }
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<T>& GetInputAView() { return m_InputAView; }
		ValueView<T>& GetInputBView() { return m_InputBView; }
		ValueView<T>& GetOutputView() { return m_OutputView; }

		const ValueView<T>& GetInputAView() const { return m_InputAView; }
		const ValueView<T>& GetInputBView() const { return m_InputBView; }
		const ValueView<T>& GetOutputView() const { return m_OutputView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "InputA" << YAML::Value << m_CurrentA;
			out << YAML::Key << "InputB" << YAML::Value << m_CurrentB;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["InputA"]) m_CurrentA = node["InputA"].as<T>();
			if (node["InputB"]) m_CurrentB = node["InputB"].as<T>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "MinNode<f32>";
			else if constexpr (std::is_same_v<T, i32>)
				return "MinNode<i32>";
			else
				return "MinNode<unknown>";
		}
	};

	// Common instantiations
	using MinNodeF = MinNode<f32>;
	using MinNodeI = MinNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			AddOutputEvent(outputEvent);

			// Initialize default values
			if constexpr (std::is_same_v<T, f32>)
			{
				m_CurrentA = 0.0f;
				m_CurrentB = 0.0f;
				m_CurrentOutput = 0.0f;
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				m_CurrentA = 0;
				m_CurrentB = 0;
				m_CurrentOutput = 0;
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);

			// Initialize ValueView streams for real-time processing
			m_InputA = CreateValueView<T>();
			m_InputB = CreateValueView<T>();
			m_Output = CreateValueView<T>();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// For simple math nodes, we typically work with single values rather than audio streams
			// But we support both modes for flexibility
			
			if (m_InputA.HasStream() && m_InputB.HasStream() && m_Output.HasStream())
			{
				// Stream processing mode - process per-sample values
				for (u32 i = 0; i < numSamples; ++i)
				{
					T valueA = m_InputA.GetNextValue();
					T valueB = m_InputB.GetNextValue();
					T result = std::min(valueA, valueB);
					m_Output.WriteValue(result);
				}
			}
			else
			{
				// Single-value processing mode - compute once and send event
				m_CurrentOutput = std::min(m_CurrentA, m_CurrentB);
				
				// Send result via output event
				auto outputEvent = FindOutputEvent("Output");
				if (outputEvent)
				{
					Value resultValue = CreateValue<T>(m_CurrentOutput);
					outputEvent->TriggerEvent(resultValue);
				}
			}
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("MinNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MinNode_i32");
			else
				return OLO_IDENTIFIER("MinNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Min (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Min (i32)";
			else
				return "Min (unknown)";
		}

		//==============================================================================
		/// Direct access methods for compatibility
		
		void SetInputA(T value) { m_CurrentA = value; }
		void SetInputB(T value) { m_CurrentB = value; }
		T GetOutput() const { return m_CurrentOutput; }
	};

	// Common type aliases
	using MinNodeF32 = MinNode<f32>;
	using MinNodeI32 = MinNode<i32>;

} // namespace OloEngine::Audio::SoundGraph