#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/FastRandom.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// RandomNode Template - Generates random values within a specified range
	/// Supports both floating-point and integer types
	/// Essential for procedural audio generation and randomized parameters
	template<typename T>
	class RandomNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, 
			"RandomNode can only be of arithmetic type (excluding bool)");

	private:
		//======================================================================
		// Parameter streams
		//======================================================================
		
		InputView<T> m_MinInput;
		InputView<T> m_MaxInput;
		InputView<i32> m_SeedInput;
		InputView<f32> m_NextInput;
		InputView<f32> m_ResetInput;
		OutputView<T> m_Output;

		// Random number generator state
		FastRandom m_Random;
		i32 m_LastSeed = -1;
		T m_LastValue = T(0);

		// Event flags for triggers
		Flag m_NextFlag;
		Flag m_ResetFlag;

		// Default values based on type
		static constexpr T GetDefaultMin() 
		{
			if constexpr (std::is_integral_v<T>) 
				return T(0);
			else 
				return T(0.0);
		}

		static constexpr T GetDefaultMax() 
		{
			if constexpr (std::is_integral_v<T>) 
				return T(100);
			else 
				return T(1.0);
		}

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		RandomNode()
		{
			// Initialize input streams with default values
			m_MinInput = CreateInputView<T>("Min", GetDefaultMin());
			m_MaxInput = CreateInputView<T>("Max", GetDefaultMax());
			m_SeedInput = CreateInputView<i32>("Seed", -1);
			m_NextInput = CreateInputView<f32>("Next", 0.0f);
			m_ResetInput = CreateInputView<f32>("Reset", 0.0f);
			
			// Initialize output stream
			m_Output = CreateOutputView<T>("Output");

			// Register input event callbacks
			m_NextInput.RegisterInputEvent([this](f32 value) {
				if (value > 0.5f) m_NextFlag.SetDirty();
			});
			
			m_ResetInput.RegisterInputEvent([this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});
		}

		virtual ~RandomNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update input parameters from connections
			m_MinInput.UpdateFromConnections();
			m_MaxInput.UpdateFromConnections();
			m_SeedInput.UpdateFromConnections();
			m_NextInput.UpdateFromConnections();
			m_ResetInput.UpdateFromConnections();
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current parameter values from streams
				T currentMin = m_MinInput.GetValue();
				T currentMax = m_MaxInput.GetValue();
				i32 currentSeed = m_SeedInput.GetValue();
				f32 nextTrigger = m_NextInput.GetValue();
				f32 resetTrigger = m_ResetInput.GetValue();

				// Check for triggers
				if (nextTrigger > 0.5f || m_NextFlag.CheckAndResetIfDirty())
				{
					GenerateNextValue(currentMin, currentMax, currentSeed);
				}

				if (resetTrigger > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
				{
					ResetRandomSeed(currentSeed);
				}

				// Set output value for this sample
				m_Output.SetValue(m_LastValue);
			}
			
			// Update output connections
			m_Output.UpdateOutputConnections();
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize random generator with default seed
			ResetRandomSeed(-1);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("RandomNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("RandomNodeI32");
			else
				return OLO_IDENTIFIER("RandomNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Random Float";
			else if constexpr (std::is_same_v<T, i32>)
				return "Random Integer";
			else
				return "Random";
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Generate a new random value within the specified range
		void GenerateNextValue(T minValue, T maxValue, i32 seed)
		{
			// Update seed if changed
			if (seed != m_LastSeed && seed != -1)
			{
				m_Random.SetSeed(seed);
				m_LastSeed = seed;
			}

			// Ensure min <= max
			if (minValue > maxValue)
			{
				std::swap(minValue, maxValue);
			}

			// Generate new value within range
			if constexpr (std::is_integral_v<T>)
			{
				if (minValue == maxValue)
					m_LastValue = minValue;
				else
					m_LastValue = m_Random.GetInt32InRange(static_cast<i32>(minValue), static_cast<i32>(maxValue));
			}
			else
			{
				if (minValue == maxValue)
					m_LastValue = minValue;
				else
					m_LastValue = m_Random.GetFloat32InRange(static_cast<f32>(minValue), static_cast<f32>(maxValue));
			}
		}

		/// Reset random seed
		void ResetRandomSeed(i32 seed)
		{
			if (seed == -1)
			{
				m_Random = FastRandom();  // Use default constructor (time-based seed)
				m_LastSeed = -1;
			}
			else
			{
				m_Random.SetSeed(seed);
				m_LastSeed = seed;
			}
			
			// Generate initial value using current parameters
			T currentMin = m_MinInput.GetValue();
			T currentMax = m_MaxInput.GetValue();
			GenerateNextValue(currentMin, currentMax, seed);
		}

		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		T GetMin() const { return m_MinInput.GetValue(); }
		void SetMin(const T& value) { m_MinInput.SetValue(value); }
		
		T GetMax() const { return m_MaxInput.GetValue(); }
		void SetMax(const T& value) { m_MaxInput.SetValue(value); }
		
		i32 GetSeed() const { return m_SeedInput.GetValue(); }
		void SetSeed(const i32& value) { m_SeedInput.SetValue(value); }
		
		T GetOutput() const { return m_LastValue; }
		
		void TriggerNext() { m_NextFlag.SetDirty(); }
		void TriggerReset() { m_ResetFlag.SetDirty(); }
		
		std::pair<T, T> GetRange() const
		{
			T minVal = m_MinInput.GetValue();
			T maxVal = m_MaxInput.GetValue();
			if (minVal > maxVal) std::swap(minVal, maxVal);
			return { minVal, maxVal };
		}
	};

	// Type aliases for common use cases
	using RandomNodeF32 = RandomNode<f32>;
	using RandomNodeI32 = RandomNode<i32>;

} // namespace OloEngine::Audio::SoundGraph