#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"
#include <cstring>
#include <cmath>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// NoiseNode - generates various types of noise (White, Pink, Brownian)
	/// Essential for audio synthesis, testing, and sound design
	class NoiseNode : public NodeProcessor
	{
	public:
		enum class NoiseType : i32
		{
			WhiteNoise = 0,
			PinkNoise = 1,
			BrownianNoise = 2
		};

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<i32> m_SeedView;
		ValueView<i32> m_TypeView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Noise Generation State
		//======================================================================
		
		i32 m_CurrentSeed = 12345;
		i32 m_CurrentType = static_cast<i32>(NoiseType::WhiteNoise);
		i32 m_LastSeed = 12345;
		NoiseType m_NoiseType = NoiseType::WhiteNoise;
		
		// Noise generation
		FastRandom m_Generator;
		
		// Pink noise state (Voss-McCartney algorithm)
		struct PinkNoiseState
		{
			f32 bins[16];
			f32 accumulation;
			u32 counter;
		} m_PinkState;
		
		// Brownian noise state
		f32 m_BrownianValue = 0.0f;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit NoiseNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_SeedView("Seed", 12345)
			, m_TypeView("Type", static_cast<i32>(NoiseType::WhiteNoise))
			, m_OutputView("Output", 0.0f)
			, m_Generator(12345)
		{
			// Create Input/Output events
			RegisterInputEvent<i32>("Seed", [this](i32 value) { m_CurrentSeed = value; });
			RegisterInputEvent<i32>("Type", [this](i32 value) { m_CurrentType = value; });
			
			RegisterOutputEvent<f32>("Output");
			
			// Initialize noise generation
			InitializeNoiseType();
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_SeedView.Initialize(maxBufferSize);
			m_TypeView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Initialize generator
			m_Generator.SetSeed(m_CurrentSeed);
			m_LastSeed = m_CurrentSeed;
			
			// Initialize noise type
			m_NoiseType = static_cast<NoiseType>(m_CurrentType);
			InitializeNoiseType();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_SeedView.UpdateFromConnections(inputs, numSamples);
			m_TypeView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				i32 seed = m_SeedView.GetValue(sample);
				i32 typeValue = m_TypeView.GetValue(sample);
				
				// Update internal state
				m_CurrentSeed = seed;
				m_CurrentType = typeValue;
				
				// Check if seed has changed
				if (seed != m_LastSeed)
				{
					m_Generator.SetSeed(seed);
					m_LastSeed = seed;
				}

				// Check if noise type has changed
				NoiseType currentType = static_cast<NoiseType>(typeValue);
				if (currentType != m_NoiseType)
				{
					m_NoiseType = currentType;
					InitializeNoiseType();
				}

				// Generate noise sample
				f32 noiseSample = GetNextNoiseValue();
				
				// Set output value
				m_OutputView.SetValue(sample, noiseSample);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("NoiseNode");
		}

		const char* GetDisplayName() const override
		{
			return "Noise Generator";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Seed")) m_CurrentSeed = static_cast<i32>(value);
			else if (id == OLO_IDENTIFIER("Type")) m_CurrentType = static_cast<i32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Seed")) return static_cast<T>(m_CurrentSeed);
			else if (id == OLO_IDENTIFIER("Type")) return static_cast<T>(m_CurrentType);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_OutputView.GetCurrentValue());
			return T{};
		}

	private:
		//======================================================================
		// Noise Generation Implementation
		//======================================================================
		
		void InitializeNoiseType()
		{
			switch (m_NoiseType)
			{
				case NoiseType::PinkNoise:
					// Initialize pink noise state
					std::memset(m_PinkState.bins, 0, sizeof(m_PinkState.bins));
					m_PinkState.accumulation = 0.0f;
					m_PinkState.counter = 1;
					break;

				case NoiseType::BrownianNoise:
					// Initialize brownian noise state
					m_BrownianValue = 0.0f;
					break;

				case NoiseType::WhiteNoise:
				default:
					// White noise requires no special initialization
					break;
			}
		}

		f32 GetNextNoiseValue()
		{
			switch (m_NoiseType)
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
			// Generate uniform random value in [-1, 1] range
			return m_Generator.GetFloat32() * 2.0f - 1.0f;
		}

		f32 GetNextPinkNoise()
		{
			// Voss-McCartney algorithm for pink noise generation
			// Count trailing zero bits for bin selection
			u32 trailingZeros = CountTrailingZeros(m_PinkState.counter);
			u32 binIndex = trailingZeros & 15; // Modulo 16

			// Update bin
			f32 binPrev = m_PinkState.bins[binIndex];
			f32 binNext = m_Generator.GetFloat32() * 2.0f - 1.0f;
			m_PinkState.bins[binIndex] = binNext;

			// Update accumulation
			m_PinkState.accumulation += (binNext - binPrev);
			++m_PinkState.counter;

			// Generate output with additional white noise
			f32 result = (m_Generator.GetFloat32() * 2.0f - 1.0f) + m_PinkState.accumulation;
			result /= 10.0f; // Scale down to reasonable range

			return std::clamp(result, -1.0f, 1.0f);
		}

		f32 GetNextBrownianNoise()
		{
			// Brownian noise (random walk)
			f32 step = (m_Generator.GetFloat32() * 2.0f - 1.0f) * 0.01f;
			m_BrownianValue += step;
			
			// Prevent unbounded drift
			m_BrownianValue *= 0.999f; 
			m_BrownianValue = std::clamp(m_BrownianValue, -1.0f, 1.0f);
			
			return m_BrownianValue;
		}

		// Count trailing zero bits utility function
		static u32 CountTrailingZeros(u32 x)
		{
			if (x == 0) return 32;
			
			u32 count = 0;
			while ((x & 1) == 0)
			{
				x >>= 1;
				++count;
			}
			return count;
		}
	};

} // namespace OloEngine::Audio::SoundGraph