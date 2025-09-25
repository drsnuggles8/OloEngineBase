#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <type_traits>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// ModuloNode - calculates the remainder of value divided by modulo (value % modulo)
	/// Supports both f32 (using fmod) and i32 operations with per-sample processing
	template<typename T>
	class ModuloNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "ModuloNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_ValueView;
		ValueView<T> m_ModuloView;
		ValueView<T> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentValue = T{};
		T m_CurrentModulo = T{2}; // Default to modulo 2

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit ModuloNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_ValueView("Value", T{})
			, m_ModuloView("Modulo", T{2})
			, m_OutputView("Result", T{})
		{
			// Create Input/Output events
			RegisterInputEvent<T>("Value", [this](const T& value) { m_CurrentValue = value; });
			RegisterInputEvent<T>("Modulo", [this](const T& value) { m_CurrentModulo = value; });
			
			RegisterOutputEvent<T>("Result");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_ValueView.Initialize(maxBufferSize);
			m_ModuloView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_ValueView.UpdateFromConnections(inputs, numSamples);
			m_ModuloView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T value = m_ValueView.GetValue(sample);
				T modulo = m_ModuloView.GetValue(sample);
				
				// Update internal state if changed
				if (value != m_CurrentValue) m_CurrentValue = value;
				if (modulo != m_CurrentModulo) m_CurrentModulo = modulo;
				
				// Calculate modulo with division-by-zero protection
				T result;
				if constexpr (std::is_same_v<T, f32>)
				{
					if (std::abs(modulo) < std::numeric_limits<f32>::epsilon())
					{
						result = 0.0f; // Safe fallback
					}
					else
					{
						result = std::fmod(value, modulo);
					}
				}
				else if constexpr (std::is_same_v<T, i32>)
				{
					if (modulo == 0)
					{
						result = 0; // Safe fallback
					}
					else
					{
						result = value % modulo;
					}
				}
				
				// Set output value
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetValue(const T& value) { TriggerInputEvent<T>("Value", value); }
		void SetModulo(const T& value) { TriggerInputEvent<T>("Modulo", value); }
		T GetResult() const 
		{ 
			if constexpr (std::is_same_v<T, f32>)
			{
				if (std::abs(m_CurrentModulo) < std::numeric_limits<f32>::epsilon())
					return 0.0f;
				return std::fmod(m_CurrentValue, m_CurrentModulo);
			}
			else
			{
				if (m_CurrentModulo == 0) return 0;
				return m_CurrentValue % m_CurrentModulo;
			}
		}
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<T>& GetValueView() { return m_ValueView; }
		ValueView<T>& GetModuloView() { return m_ModuloView; }
		ValueView<T>& GetResultView() { return m_OutputView; }

		const ValueView<T>& GetValueView() const { return m_ValueView; }
		const ValueView<T>& GetModuloView() const { return m_ModuloView; }
		const ValueView<T>& GetResultView() const { return m_OutputView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "Value" << YAML::Value << m_CurrentValue;
			out << YAML::Key << "Modulo" << YAML::Value << m_CurrentModulo;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["Value"]) m_CurrentValue = node["Value"].as<T>();
			if (node["Modulo"]) m_CurrentModulo = node["Modulo"].as<T>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "ModuloNode<f32>";
			else if constexpr (std::is_same_v<T, i32>)
				return "ModuloNode<i32>";
			else
				return "ModuloNode<unknown>";
		}
	};

	// Common instantiations
	using ModuloNodeF = ModuloNode<f32>;
	using ModuloNodeI = ModuloNode<i32>;

} // namespace OloEngine::Audio::SoundGraph