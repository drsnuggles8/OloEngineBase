#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
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
		// Endpoint identifiers
		const Identifier Min_ID = OLO_IDENTIFIER("Min");
		const Identifier Max_ID = OLO_IDENTIFIER("Max");
		const Identifier Seed_ID = OLO_IDENTIFIER("Seed");
		const Identifier Next_ID = OLO_IDENTIFIER("Next");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Random number generator state
		FastRandom m_Random;
		i32 m_CurrentSeed = -1;
		T m_LastValue = T(0);

		// Event flags
		Flag m_NextFlag;
		Flag m_ResetFlag;

		// Output events
		std::shared_ptr<OutputEvent> m_NextEvent;
		std::shared_ptr<OutputEvent> m_ResetEvent;

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
		RandomNode()
		{
			// Register parameters with appropriate defaults
			AddParameter<T>(Min_ID, "Min", GetDefaultMin());
			AddParameter<T>(Max_ID, "Max", GetDefaultMax());
			AddParameter<i32>(Seed_ID, "Seed", -1);  // -1 means use time-based seed
			AddParameter<f32>(Next_ID, "Next", 0.0f);  // Next trigger
			AddParameter<f32>(Reset_ID, "Reset", 0.0f);  // Reset trigger
			AddParameter<T>(Output_ID, "Output", T(0));

			// Register input events with flag callbacks
			AddInputEvent<f32>(Next_ID, "Next", [this](f32 value) {
				if (value > 0.5f) m_NextFlag.SetDirty();
			});
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});

			// Register output events
			m_NextEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnNext"), "OnNext");
			m_ResetEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnReset"), "OnReset");
		}

		virtual ~RandomNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check for triggers via parameters or flags
			f32 nextValue = GetParameterValue<f32>(Next_ID);
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			
			if (nextValue > 0.5f || m_NextFlag.CheckAndResetIfDirty())
			{
				GenerateNextValue();
				// Fire Next event
				if (m_NextEvent)
					(*m_NextEvent)(1.0f);
				// Reset trigger parameter
				if (nextValue > 0.5f)
					SetParameterValue(Next_ID, 0.0f);
			}

			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				ResetRandomSeed();
				GenerateNextValue(); // Generate new value after reset
				// Fire Reset event
				if (m_ResetEvent)
					(*m_ResetEvent)(1.0f);
				// Reset trigger parameter
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f);
			}

			// Fill output buffer with current value (constant)
			if (outputs && outputs[0])
			{
				const f32 outputValue = static_cast<f32>(m_LastValue);
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = outputValue;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize random generator
			const i32 seed = GetParameterValue<i32>(Seed_ID);
			m_CurrentSeed = seed;
			
			if (seed == -1)
			{
				m_Random = FastRandom();  // Use default constructor (time-based seed)
			}
			else
			{
				m_Random.SetSeed(seed);
			}

			// Pure event-driven: no automatic value generation
			// Values must be explicitly requested via Next event or parameter
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

		/// Generate a new random value and update output
		void GenerateNextValue()
		{
			const T minValue = GetParameterValue<T>(Min_ID);
			const T maxValue = GetParameterValue<T>(Max_ID);
			const i32 seed = GetParameterValue<i32>(Seed_ID);

			// Initialize or re-seed if needed
			if (seed != m_CurrentSeed)
			{
				m_CurrentSeed = seed;
				if (seed == -1)
				{
					// Use time-based seed (FastRandom generates its own default)
					m_Random = FastRandom();
				}
				else
				{
					// Use specified seed
					m_Random.SetSeed(seed);
				}
			}

			// Ensure min <= max
			T actualMin = minValue;
			T actualMax = maxValue;
			if (actualMin > actualMax)
			{
				std::swap(actualMin, actualMax);
			}

			// Generate random value
			T randomValue;
			if constexpr (std::is_integral_v<T>)
			{
				// Integer types: use uniform distribution
				if (actualMin == actualMax)
				{
					randomValue = actualMin;
				}
				else
				{
					randomValue = m_Random.GetInt32InRange(static_cast<i32>(actualMin), static_cast<i32>(actualMax));
				}
			}
			else
			{
				// Floating-point types: use uniform distribution
				if (actualMin == actualMax)
				{
					randomValue = actualMin;
				}
				else
				{
					randomValue = m_Random.GetFloat32InRange(static_cast<f32>(actualMin), static_cast<f32>(actualMax));
				}
			}

			m_LastValue = randomValue;
			SetParameterValue(Output_ID, randomValue);
		}

		/// Reset the random generator with current or new seed
		void ResetRandomSeed()
		{
			const i32 seed = GetParameterValue<i32>(Seed_ID);
			m_CurrentSeed = seed;
			
			if (seed == -1)
			{
				m_Random = FastRandom();
			}
			else
			{
				m_Random.SetSeed(seed);
			}
		}

		/// Generate a new random value (useful for triggering updates)
		T GenerateNext()
		{
			GenerateNextValue();
			return m_LastValue;
		}

		/// Get the last generated value
		T GetLastValue() const
		{
			return m_LastValue;
		}

		/// Reset the random generator with a new seed
		void ResetSeed(i32 newSeed = -1)
		{
			SetParameterValue(Seed_ID, newSeed);
			m_CurrentSeed = newSeed;
			
			if (newSeed == -1)
			{
				m_Random = FastRandom();
			}
			else
			{
				m_Random.SetSeed(newSeed);
			}
		}

		/// Get the current range
		std::pair<T, T> GetRange() const
		{
			T minVal = GetParameterValue<T>(Min_ID);
			T maxVal = GetParameterValue<T>(Max_ID);
			if (minVal > maxVal) std::swap(minVal, maxVal);
			return { minVal, maxVal };
		}
	};

	// Type aliases for common use cases
	using RandomNodeF32 = RandomNode<f32>;
	using RandomNodeI32 = RandomNode<i32>;

} // namespace OloEngine::Audio::SoundGraph