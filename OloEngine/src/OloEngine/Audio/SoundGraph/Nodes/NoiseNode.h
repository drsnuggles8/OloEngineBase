#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/FastRandom.h"
#include <cstring>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Noise Node - generates various types of noise (White, Pink, Brownian)
	/// Essential for audio synthesis, testing, and sound design
	class NoiseNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Seed_ID = OLO_IDENTIFIER("Seed");
		const Identifier Type_ID = OLO_IDENTIFIER("Type");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		enum class NoiseType : i32
		{
			WhiteNoise = 0,
			PinkNoise = 1,
			BrownianNoise = 2
		};

		NoiseNode()
		{
			// Register parameters directly
			AddParameter<i32>(Seed_ID, "Seed", 12345);
			AddParameter<i32>(Type_ID, "Type", static_cast<i32>(NoiseType::WhiteNoise));
			AddParameter<f32>(Output_ID, "Output", 0.0f);
			
			// Initialize noise generation
			InitializeNoiseType();
		}

		virtual ~NoiseNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Check if seed has changed
			const i32 currentSeed = GetParameterValue<i32>(Seed_ID);
			if (currentSeed != m_LastSeed)
			{
				m_Generator.SetSeed(currentSeed);
				m_LastSeed = currentSeed;
			}

			// Check if noise type has changed
			const i32 typeValue = GetParameterValue<i32>(Type_ID);
			const NoiseType currentType = static_cast<NoiseType>(typeValue);
			if (currentType != m_CurrentType)
			{
				m_CurrentType = currentType;
				InitializeNoiseType();
			}

			// Generate noise samples and set output parameter with last sample
			f32 lastSample = 0.0f;
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					lastSample = GetNextNoiseValue();
					outputs[0][i] = lastSample;
				}
			}
			else
			{
				// Still generate one sample for parameter output even if no buffer
				lastSample = GetNextNoiseValue();
			}
			
			// Set output parameter
			SetParameterValue(Output_ID, lastSample);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize the generator with the current seed
			const i32 seed = GetParameterValue<i32>(Seed_ID);
			m_Generator.SetSeed(seed);
			m_LastSeed = seed;

			// Initialize noise type
			const i32 typeValue = GetParameterValue<i32>(Type_ID);
			m_CurrentType = static_cast<NoiseType>(typeValue);
			InitializeNoiseType();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("NoiseNode");
		}

		const char* GetDisplayName() const override
		{
			return "Noise Generator";
		}

	private:
		void InitializeNoiseType()
		{
			switch (m_CurrentType)
			{
			case NoiseType::PinkNoise:
				// Initialize pink noise state
				std::memset(m_PinkState.bins, 0, sizeof(m_PinkState.bins));
				m_PinkState.accumulation = 0.0;
				m_PinkState.counter = 1;
				break;

			case NoiseType::BrownianNoise:
				// Initialize brownian noise state
				m_BrownianState.accumulation = 0.0;
				break;

			case NoiseType::WhiteNoise:
			default:
				// White noise requires no special initialization
				break;
			}
		}

		f32 GetNextNoiseValue()
		{
			switch (m_CurrentType)
			{
			case NoiseType::WhiteNoise:
				return GetNextWhiteNoise();
			case NoiseType::PinkNoise:
				return GetNextPinkNoise();
			case NoiseType::BrownianNoise:
				return GetNextBrownianNoise();
			default:
				return GetNextWhiteNoise();
			}
		}

		f32 GetNextWhiteNoise()
		{
			return m_Generator.GetFloat32();
		}

		f32 GetNextPinkNoise()
		{
			// Count trailing zero bits for bin selection
			const u32 trailingZeros = CountTrailingZeros(m_PinkState.counter);
			const u32 binIndex = trailingZeros & (PINK_NOISE_BIN_SIZE - 1);

			// Update bin
			const f64 binPrev = m_PinkState.bins[binIndex];
			const f64 binNext = m_Generator.GetFloat64();
			m_PinkState.bins[binIndex] = binNext;

			// Update accumulation
			m_PinkState.accumulation += (binNext - binPrev);
			++m_PinkState.counter;

			// Generate output
			f64 result = m_Generator.GetFloat64() + m_PinkState.accumulation;
			result /= 10.0; // Scale down

			return static_cast<f32>(result);
		}

		f32 GetNextBrownianNoise()
		{
			f64 result = m_Generator.GetFloat64() + m_BrownianState.accumulation;
			result /= 1.005; // Prevent escaping -1..1 range on average
			
			m_BrownianState.accumulation = result;
			result /= 20.0; // Scale down
			
			return static_cast<f32>(result);
		}

		// Count trailing zero bits (equivalent to Hazel's Tzcnt32)
		static u32 CountTrailingZeros(u32 x)
		{
			if (x & 0x1) return 0;
			if (x == 0) return 32;

			u32 n = 1;
			if ((x & 0x0000FFFF) == 0) { x >>= 16; n += 16; }
			if ((x & 0x000000FF) == 0) { x >>= 8; n += 8; }
			if ((x & 0x0000000F) == 0) { x >>= 4; n += 4; }
			if ((x & 0x00000003) == 0) { x >>= 2; n += 2; }
			n -= x & 0x00000001;

			return n;
		}

	private:
		static constexpr u32 PINK_NOISE_BIN_SIZE = 16;

		// Generator state
		FastRandom m_Generator;
		i32 m_LastSeed = -1;
		NoiseType m_CurrentType = NoiseType::WhiteNoise;

		// Pink noise state
		struct PinkNoiseState
		{
			f64 bins[PINK_NOISE_BIN_SIZE];
			f64 accumulation;
			u32 counter;
		} m_PinkState;

		// Brownian noise state
		struct BrownianNoiseState
		{
			f64 accumulation;
		} m_BrownianState;
	};

} // namespace OloEngine::Audio::SoundGraph