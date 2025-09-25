#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Multiply node - Simple multiplication of two values using new Hazel-style foundation
	/// Supports both real-time audio streams and single-value processing
	template<typename T>
	class MultiplyNode : public NodeProcessor
	{
	private:
		// Input streams for connected values
		ValueView<T> m_InputA;
		ValueView<T> m_InputB;
		
		// Output stream for computed results
		ValueView<T> m_Output;

		// Current values (for single-value processing)
		T m_CurrentA{};
		T m_CurrentB{};
		T m_CurrentOutput{};

	public:
		MultiplyNode()
		{
			// Create input events for receiving values
			auto inputAEvent = std::make_shared<InputEvent>("InputA", [this](const Value& value) {
				if (value.GetType() == GetValueType<T>())
				{
					m_CurrentA = value.Get<T>();
				}
			});
			
			auto inputBEvent = std::make_shared<InputEvent>("InputB", [this](const Value& value) {
				if (value.GetType() == GetValueType<T>())
				{
					m_CurrentB = value.Get<T>();
				}
			});

			// Register input events
			AddInputEvent(inputAEvent);
			AddInputEvent(inputBEvent);

			// Create output event for sending computed values
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			AddOutputEvent(outputEvent);

			// Initialize default values
			if constexpr (std::is_same_v<T, f32>)
			{
				m_CurrentA = 1.0f;  // Multiply identity
				m_CurrentB = 1.0f;
				m_CurrentOutput = 1.0f;
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				m_CurrentA = 1;  // Multiply identity
				m_CurrentB = 1;
				m_CurrentOutput = 1;
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
					T result = valueA * valueB;
					m_Output.WriteValue(result);
				}
			}
			else
			{
				// Single-value processing mode - compute once and send event
				m_CurrentOutput = m_CurrentA * m_CurrentB;
				
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
				return OLO_IDENTIFIER("MultiplyNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MultiplyNode_i32");
			else
				return OLO_IDENTIFIER("MultiplyNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Multiply (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Multiply (i32)";
			else
				return "Multiply (unknown)";
		}

		//==============================================================================
		/// Direct access methods for compatibility
		
		void SetInputA(T value) { m_CurrentA = value; }
		void SetInputB(T value) { m_CurrentB = value; }
		T GetOutput() const { return m_CurrentOutput; }
	};

	// Common type aliases
	using MultiplyNodeF32 = MultiplyNode<f32>;
	using MultiplyNodeI32 = MultiplyNode<i32>;

} // namespace OloEngine::Audio::SoundGraph