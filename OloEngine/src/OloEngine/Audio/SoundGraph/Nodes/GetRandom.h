#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include "OloEngine/Core/FastRandom.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// GetRandom - Template node for random element selection from arrays
	/// Converts from legacy parameters to ValueView system while preserving functionality
	template<typename T>
	class GetRandom : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GetRandom can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<i32> m_SeedView;
		InputView<f32> m_NextView;
		InputView<f32> m_ResetView;
		InputView<f32> m_NoRepeatsView;
		
		// Output streams
		OutputView<T> m_OutputView;
		OutputView<T> m_SelectedView;
		
		// Current parameter values for legacy API compatibility
		i32 m_CurrentSeed = 0;
		f32 m_CurrentNext = 0.0f;
		f32 m_CurrentReset = 0.0f;
		f32 m_CurrentNoRepeats = 0.0f;
		T m_CurrentOutput{};
		T m_CurrentSelected{};

		//======================================================================
		// Random Generation State
		//======================================================================
		
		std::vector<T> m_Array;
		FastRandom m_Random;
		i32 m_LastSelectedIndex = -1;  // Track last selected index for NoRepeats
		
		// Previous sample values for edge detection
		f32 m_PreviousNext = 0.0f;
		f32 m_PreviousReset = 0.0f;
		
		// Trigger threshold for digital logic
		static constexpr f32 TRIGGER_THRESHOLD = 0.5f;

	public:
		GetRandom()
			: m_SeedView([this](i32 value) { m_CurrentSeed = value; }),
			  m_NextView([this](f32 value) { m_CurrentNext = value; }),
			  m_ResetView([this](f32 value) { m_CurrentReset = value; }),
			  m_NoRepeatsView([this](f32 value) { m_CurrentNoRepeats = value; }),
			  m_OutputView([this](T value) { m_CurrentOutput = value; }),
			  m_SelectedView([this](T value) { m_CurrentSelected = value; })
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
			m_SeedView.Initialize(maxBufferSize);
			m_NextView.Initialize(maxBufferSize);
			m_ResetView.Initialize(maxBufferSize);
			m_NoRepeatsView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			m_SelectedView.Initialize(maxBufferSize);
			
			// Initialize random generator
			if (m_CurrentSeed == 0)
			{
				m_Random.SetSeed(RandomUtils::GetTimeBasedSeed());
			}
			else
			{
				m_Random.SetSeed(m_CurrentSeed);
			}
			
			// Initialize state
			m_PreviousNext = 0.0f;
			m_PreviousReset = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_SeedView.UpdateFromConnections(inputs, numSamples);
			m_NextView.UpdateFromConnections(inputs, numSamples);
			m_ResetView.UpdateFromConnections(inputs, numSamples);
			m_NoRepeatsView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				i32 seedValue = m_SeedView.GetValue(sample);
				f32 nextValue = m_NextView.GetValue(sample);
				f32 resetValue = m_ResetView.GetValue(sample);
				f32 noRepeatsValue = m_NoRepeatsView.GetValue(sample);
				
				// Update internal state
				m_CurrentSeed = seedValue;
				m_CurrentNext = nextValue;
				m_CurrentReset = resetValue;
				m_CurrentNoRepeats = noRepeatsValue;
				
				// Detect edges (rising edge detection)
				bool nextEdge = nextValue > TRIGGER_THRESHOLD && m_PreviousNext <= TRIGGER_THRESHOLD;
				bool resetEdge = resetValue > TRIGGER_THRESHOLD && m_PreviousReset <= TRIGGER_THRESHOLD;
				
				// Handle reset first (takes priority)
				if (resetEdge)
				{
					ResetSeed();
				}
				
				// Handle next trigger
				if (nextEdge)
				{
					SelectRandomElement(sample, noRepeatsValue > TRIGGER_THRESHOLD);
				}
				else
				{
					// Output current values even without trigger
					m_OutputView.SetValue(sample, m_CurrentOutput);
					m_SelectedView.SetValue(sample, m_CurrentSelected);
				}
				
				// Update previous values for edge detection
				m_PreviousNext = nextValue;
				m_PreviousReset = resetValue;
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
			m_SelectedView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
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

		// Legacy parameter methods for compatibility
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("Seed")) m_CurrentSeed = static_cast<i32>(value);
			else if (id == OLO_IDENTIFIER("Next")) m_CurrentNext = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Reset")) m_CurrentReset = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("NoRepeats")) m_CurrentNoRepeats = static_cast<f32>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Seed")) return static_cast<U>(m_CurrentSeed);
			else if (id == OLO_IDENTIFIER("Next")) return static_cast<U>(m_CurrentNext);
			else if (id == OLO_IDENTIFIER("Reset")) return static_cast<U>(m_CurrentReset);
			else if (id == OLO_IDENTIFIER("NoRepeats")) return static_cast<U>(m_CurrentNoRepeats);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			else if (id == OLO_IDENTIFIER("Selected")) return static_cast<U>(m_CurrentSelected);
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

		/// Get the current random seed
		i32 GetCurrentSeed() const
		{
			return m_Random.GetCurrentSeed();
		}

		/// Manually trigger random selection
		void ManualNext()
		{
			SelectRandomElement(0, m_CurrentNoRepeats > TRIGGER_THRESHOLD);
		}

		/// Manually reset the random generator
		void ManualReset()
		{
			ResetSeed();
		}

	private:
		void SelectRandomElement(u32 sample, bool noRepeats)
		{
			if (m_Array.empty())
			{
				m_OutputView.SetValue(sample, T{});
				m_SelectedView.SetValue(sample, T{});
				return;
			}

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

			// Update current values
			m_CurrentOutput = selectedValue;
			m_CurrentSelected = selectedValue;

			// Set output values for this sample
			m_OutputView.SetValue(sample, selectedValue);
			m_SelectedView.SetValue(sample, selectedValue);
		}

		void ResetSeed()
		{
			// Re-seed the random generator and reset last selected index
			if (m_CurrentSeed == 0)
			{
				m_Random.SetSeed(RandomUtils::GetTimeBasedSeed());
			}
			else
			{
				m_Random.SetSeed(m_CurrentSeed);
			}
			
			// Reset the last selected index when resetting
			m_LastSelectedIndex = -1;
		}
	};

	// Type aliases for common usage
	using GetRandomF32 = GetRandom<f32>;
	using GetRandomI32 = GetRandom<i32>;

} // namespace OloEngine::Audio::SoundGraph