#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Get - Template node for indexed array access with modulo wraparound
	/// Converts from legacy parameters to ValueView system while preserving functionality
	template<typename T>
	class Get : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "Get can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_IndexView;
		InputView<f32> m_TriggerView;
		
		// Output streams
		OutputView<T> m_OutputView;
		OutputView<T> m_ElementView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentIndex = 0.0f;
		f32 m_CurrentTrigger = 0.0f;
		T m_CurrentOutput{};
		T m_CurrentElement{};

		//======================================================================
		// Array State
		//======================================================================
		
		std::vector<T> m_Array;
		
		// Previous sample value for edge detection
		f32 m_PreviousTrigger = 0.0f;
		
		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;

	public:
		Get()
			: m_IndexView([this](f32 value) { m_CurrentIndex = value; }),
			  m_TriggerView([this](f32 value) { m_CurrentTrigger = value; }),
			  m_OutputView([this](T value) { m_CurrentOutput = value; }),
			  m_ElementView([this](T value) { m_CurrentElement = value; })
		{
			// Set up default array with some test values
			if constexpr (std::is_same_v<T, f32>)
			{
				m_Array = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				m_Array = {0, 1, 2, 3, 4};
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_IndexView.Initialize(maxBufferSize);
			m_TriggerView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			m_ElementView.Initialize(maxBufferSize);
			
			// Initialize state
			m_PreviousTrigger = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_IndexView.UpdateFromConnections(inputs, numSamples);
			m_TriggerView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 indexValue = m_IndexView.GetValue(sample);
				f32 triggerValue = m_TriggerView.GetValue(sample);
				
				// Update internal state
				m_CurrentIndex = indexValue;
				m_CurrentTrigger = triggerValue;
				
				// Detect trigger edge (rising edge detection)
				bool triggerEdge = triggerValue > TRIGGER_THRESHOLD && m_PreviousTrigger <= TRIGGER_THRESHOLD;
				
				if (triggerEdge)
				{
					GetElement(sample, indexValue);
				}
				else
				{
					// Output current values even without trigger
					m_OutputView.SetValue(sample, m_CurrentOutput);
					m_ElementView.SetValue(sample, m_CurrentElement);
				}
				
				// Update previous value for edge detection
				m_PreviousTrigger = triggerValue;
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
			m_ElementView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("Get_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("Get_i32");
			else
				return OLO_IDENTIFIER("Get_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Get (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Get (i32)";
			else
				return "Get (unknown)";
		}

		// Legacy parameter methods for compatibility
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("Index")) m_CurrentIndex = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Trigger")) m_CurrentTrigger = static_cast<f32>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Index")) return static_cast<U>(m_CurrentIndex);
			else if (id == OLO_IDENTIFIER("Trigger")) return static_cast<U>(m_CurrentTrigger);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			else if (id == OLO_IDENTIFIER("Element")) return static_cast<U>(m_CurrentElement);
			return U{};
		}

		//======================================================================
		// Array Management Methods
		//======================================================================
		
		/// Set the entire array
		void SetArray(const std::vector<T>& array)
		{
			m_Array = array;
		}

		/// Get a reference to the array
		const std::vector<T>& GetArray() const
		{
			return m_Array;
		}

		/// Add an element to the array
		void AddElement(T element)
		{
			m_Array.push_back(element);
		}

		/// Clear all elements from the array
		void ClearArray()
		{
			m_Array.clear();
		}

		/// Get the current size of the array
		sizet GetArraySize() const
		{
			return m_Array.size();
		}

		/// Set an element at a specific index (with bounds checking)
		void SetElement(sizet index, T value)
		{
			if (index < m_Array.size())
			{
				m_Array[index] = value;
			}
		}

		/// Get an element at a specific index (with bounds checking)
		T GetElementAt(sizet index) const
		{
			if (index < m_Array.size())
			{
				return m_Array[index];
			}
			return T{};
		}

		/// Manually trigger array access with current index
		void ManualTrigger()
		{
			GetElement(0, m_CurrentIndex);
		}

	private:
		void GetElement(u32 sample, f32 indexFloat)
		{
			if (m_Array.empty())
			{
				m_OutputView.SetValue(sample, T{});
				m_ElementView.SetValue(sample, T{});
				return;
			}

			// Get index with modulo wraparound for bounds safety
			i32 index = static_cast<i32>(indexFloat);
			i32 arraySize = static_cast<i32>(m_Array.size());
			
			// Handle negative indices with proper wraparound
			index = index % arraySize;
			if (index < 0)
			{
				index += arraySize;
			}
			
			T element = m_Array[index];

			// Update current values
			m_CurrentOutput = element;
			m_CurrentElement = element;

			// Set output values for this sample
			m_OutputView.SetValue(sample, element);
			m_ElementView.SetValue(sample, element);
		}
	};

	// Type aliases for common usage
	using GetF32 = Get<f32>;
	using GetI32 = Get<i32>;

} // namespace OloEngine::Audio::SoundGraph