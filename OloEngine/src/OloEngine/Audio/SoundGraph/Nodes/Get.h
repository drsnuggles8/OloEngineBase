#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Get - Template node for indexed array access with modulo wraparound
	/// Based on Hazel's Get node
	template<typename T>
	class Get : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "Get can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier Index_ID = OLO_IDENTIFIER("Index");
		const Identifier Trigger_ID = OLO_IDENTIFIER("Trigger");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Element_ID = OLO_IDENTIFIER("Element");

		// Array state
		std::vector<T> m_Array;

		// Flag for parameter change detection
		Flag m_TriggerFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_OutputEvent;
		std::shared_ptr<OutputEvent> m_ElementEvent;

	public:
		Get()
		{
			// Register parameters
			AddParameter<f32>(Index_ID, "Index", 0.0f);
			AddParameter<f32>(Trigger_ID, "Trigger", 0.0f);
			
			// Add output parameters for the selected values
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(Output_ID, "Output", 0.0f);
				AddParameter<f32>(Element_ID, "Element", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(Output_ID, "Output", 0);
				AddParameter<i32>(Element_ID, "Element", 0);
			}

			// Register input event with flag callback
			AddInputEvent<f32>(Trigger_ID, "Trigger", [this](f32 value) {
				if (value > 0.5f) m_TriggerFlag.SetDirty();
			});

			// Register output events
			m_OutputEvent = AddOutputEvent<T>(Output_ID, "Output");
			m_ElementEvent = AddOutputEvent<T>(Element_ID, "Element");

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
			m_SampleRate = sampleRate;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for trigger via parameter or flag
			f32 triggerValue = GetParameterValue<f32>(Trigger_ID);
			
			if (triggerValue > 0.5f || m_TriggerFlag.CheckAndResetIfDirty())
			{
				GetElement();
				// Reset trigger parameter
				if (triggerValue > 0.5f)
					SetParameterValue(Trigger_ID, 0.0f);
			}
		}

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

		// Array management methods
		void SetArray(const std::vector<T>& array)
		{
			m_Array = array;
		}

		const std::vector<T>& GetArray() const
		{
			return m_Array;
		}

		void AddElement(T element)
		{
			m_Array.push_back(element);
		}

		void ClearArray()
		{
			m_Array.clear();
		}

		sizet GetArraySize() const
		{
			return m_Array.size();
		}

	private:
		void GetElement()
		{
			if (m_Array.empty())
				return;

			// Get index with modulo wraparound for bounds safety
			f32 indexFloat = GetParameterValue<f32>(Index_ID, 0.0f);
			i32 index = static_cast<i32>(indexFloat);
			i32 arraySize = static_cast<i32>(m_Array.size());
			
			// Handle negative indices with proper wraparound
			// In C++, negative modulo doesn't work as expected, so we need to handle it manually
			index = index % arraySize;
			if (index < 0)
			{
				index += arraySize;
			}
			
			T element = m_Array[index];

			// Update output parameters
			SetParameterValue(Output_ID, element);
			SetParameterValue(Element_ID, element);

			// Fire output events
			if (m_OutputEvent)
				(*m_OutputEvent)(element);
			if (m_ElementEvent)
				(*m_ElementEvent)(element);
		}
	};

	// Type aliases for common usage
	using GetF32 = Get<f32>;
	using GetI32 = Get<i32>;
}