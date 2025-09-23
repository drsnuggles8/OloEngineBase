#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"
#include "OloEngine/Core/FastRandom.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// GetRandom - Template node for random element selection from arrays
	/// Based on Hazel's GetRandom node
	template<typename T>
	class GetRandom : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GetRandom can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier Seed_ID = OLO_IDENTIFIER("Seed");
		const Identifier Next_ID = OLO_IDENTIFIER("Next");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier NoRepeats_ID = OLO_IDENTIFIER("NoRepeats");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier Selected_ID = OLO_IDENTIFIER("Selected");

		// Array and random state
		std::vector<T> m_Array;
		FastRandom m_Random;
		i32 m_LastSelectedIndex = -1;  // Track last selected index for NoRepeats

		// Flags for parameter change detection
		Flag m_NextFlag;
		Flag m_ResetFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_OutputEvent;
		std::shared_ptr<OutputEvent> m_SelectedEvent;

	public:
		GetRandom()
		{
			// Register parameters
			AddParameter<i32>(Seed_ID, "Seed", 0);
			AddParameter<f32>(Next_ID, "Next", 0.0f);
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);
			AddParameter<f32>(NoRepeats_ID, "NoRepeats", 0.0f);  // 0 = allow repeats, 1 = no repeats
			
			// Add output parameters for the selected values
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(Output_ID, "Output", 0.0f);
				AddParameter<f32>(Selected_ID, "Selected", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(Output_ID, "Output", 0);
				AddParameter<i32>(Selected_ID, "Selected", 0);
			}

			// Register input events with flag callbacks
			AddInputEvent<f32>(Next_ID, "Next", [this](f32 value) {
				if (value > 0.5f) m_NextFlag.SetDirty();
			});
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output events
			m_OutputEvent = AddOutputEvent<T>(Output_ID, "Output");
			m_SelectedEvent = AddOutputEvent<T>(Selected_ID, "Selected");

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
			
			// Initialize random generator
			i32 seed = GetParameterValue<i32>(Seed_ID, 0);
			if (seed == 0)
			{
				m_Random.SetSeed(RandomUtils::GetTimeBasedSeed());
			}
			else
			{
				m_Random.SetSeed(seed);
			}
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for triggers via parameter or flag
			f32 nextValue = GetParameterValue<f32>(Next_ID);
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			
			if (nextValue > 0.5f || m_NextFlag.CheckAndResetIfDirty())
			{
				SelectRandomElement();
				// Reset trigger parameter
				if (nextValue > 0.5f)
					SetParameterValue(Next_ID, 0.0f);
			}

			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				ResetSeed();
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("GetRandom_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("GetRandom_i32");
			else
				return OLO_IDENTIFIER("GetRandom_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Get Random (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Get Random (i32)";
			else
				return "Get Random (unknown)";
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

	private:
		void SelectRandomElement()
		{
			if (m_Array.empty())
				return;

			const bool noRepeats = GetParameterValue<f32>(NoRepeats_ID) > 0.5f;
			i32 index;

			if (noRepeats && m_Array.size() > 1 && m_LastSelectedIndex != -1)
			{
				// NoRepeats mode: ensure we don't select the same index as last time
				do
				{
					index = m_Random.GetInt32InRange(0, static_cast<i32>(m_Array.size() - 1));
				} while (index == m_LastSelectedIndex);
			}
			else
			{
				// Normal mode: any index is valid
				index = m_Random.GetInt32InRange(0, static_cast<i32>(m_Array.size() - 1));
			}

			// Store the selected index for NoRepeats functionality
			m_LastSelectedIndex = index;
			T selectedValue = m_Array[index];

			// Update output parameters
			SetParameterValue(Output_ID, selectedValue);
			SetParameterValue(Selected_ID, selectedValue);

			// Fire output events
			if (m_OutputEvent)
				(*m_OutputEvent)(selectedValue);
			if (m_SelectedEvent)
				(*m_SelectedEvent)(selectedValue);
		}

		void ResetSeed()
		{
			// Re-seed the random generator and reset last selected index
			i32 seed = GetParameterValue<i32>(Seed_ID, 0);
			if (seed == 0)
			{
				m_Random.SetSeed(RandomUtils::GetTimeBasedSeed());
			}
			else
			{
				m_Random.SetSeed(seed);
			}
			
			// Reset the last selected index when resetting
			m_LastSelectedIndex = -1;
		}
	};

	// Type aliases for common usage
	using GetRandomF32 = GetRandom<f32>;
	using GetRandomI32 = GetRandom<i32>;
}