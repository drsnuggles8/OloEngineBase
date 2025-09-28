#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/FastRandom.h"

#include <glm/glm.hpp>
#include <numbers>
#include <cmath>
#include <ctime>
#include <cstring>

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	// Sine Wave Oscillator
	//==============================================================================
	struct SineOscillator : public NodeProcessor
	{
		explicit SineOscillator(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		// Input parameters
		float* in_Frequency = nullptr;		// Frequency in Hz
		float* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		float* in_Phase = nullptr;			// Phase offset in radians

		// Output
		float out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0f;
		}

		void Process() final
		{
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;

			// Update phase
			float deltaPhase = frequency / m_SampleRate;
			m_Phase += deltaPhase;
			
			// Wrap phase to [0, 1]
			if (m_Phase >= 1.0f)
				m_Phase = fmod(m_Phase, 1.0f);
			
			// Calculate sine with phase offset
			float totalPhase = m_Phase + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			totalPhase = fmod(totalPhase + 1.0f, 1.0f); // Ensure positive
			
			out_Value = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * totalPhase);
		}

	private:
		float m_Phase{ 0.0f };
	};

	//==============================================================================
	// Square Wave Oscillator  
	//==============================================================================
	struct SquareOscillator : public NodeProcessor
	{
		explicit SquareOscillator(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		// Input parameters
		float* in_Frequency = nullptr;		// Frequency in Hz
		float* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		float* in_Phase = nullptr;			// Phase offset in radians
		float* in_PulseWidth = nullptr;		// Pulse width (0.0 to 1.0, 0.5 = square)

		// Output
		float out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0f;
		}

		void Process() final
		{
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;
			float pulseWidth = glm::clamp(*in_PulseWidth, 0.01f, 0.99f);

			// Update phase
			float deltaPhase = frequency / m_SampleRate;
			m_Phase += deltaPhase;
			
			// Wrap phase to [0, 1]
			if (m_Phase >= 1.0f)
				m_Phase = fmod(m_Phase, 1.0f);
			
			// Calculate square with phase offset and pulse width
			float totalPhase = m_Phase + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			totalPhase = fmod(totalPhase + 1.0f, 1.0f); // Ensure positive
			
			out_Value = amplitude * (totalPhase < pulseWidth ? 1.0f : -1.0f);
		}

	private:
		float m_Phase{ 0.0f };
	};

	//==============================================================================
	// Sawtooth Wave Oscillator
	//==============================================================================
	struct SawtoothOscillator : public NodeProcessor
	{
		explicit SawtoothOscillator(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		// Input parameters
		float* in_Frequency = nullptr;		// Frequency in Hz
		float* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		float* in_Phase = nullptr;			// Phase offset in radians

		// Output
		float out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0f;
		}

		void Process() final
		{
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;

			// Update phase
			float deltaPhase = frequency / m_SampleRate;
			m_Phase += deltaPhase;
			
			// Wrap phase to [0, 1]
			if (m_Phase >= 1.0f)
				m_Phase = fmod(m_Phase, 1.0f);
			
			// Calculate sawtooth with phase offset
			float totalPhase = m_Phase + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			totalPhase = fmod(totalPhase + 1.0f, 1.0f); // Ensure positive
			
			// Convert [0,1] to [-1,1] sawtooth
			out_Value = amplitude * (2.0f * totalPhase - 1.0f);
		}

	private:
		float m_Phase{ 0.0f };
	};

	//==============================================================================
	// Triangle Wave Oscillator
	//==============================================================================
	struct TriangleOscillator : public NodeProcessor
	{
		explicit TriangleOscillator(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		// Input parameters
		float* in_Frequency = nullptr;		// Frequency in Hz
		float* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		float* in_Phase = nullptr;			// Phase offset in radians

		// Output
		float out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0f;
		}

		void Process() final
		{
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;

			// Update phase
			float deltaPhase = frequency / m_SampleRate;
			m_Phase += deltaPhase;
			
			// Wrap phase to [0, 1]
			if (m_Phase >= 1.0f)
				m_Phase = fmod(m_Phase, 1.0f);
			
			// Calculate triangle with phase offset
			float totalPhase = m_Phase + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			totalPhase = fmod(totalPhase + 1.0f, 1.0f); // Ensure positive
			
			// Convert [0,1] to [-1,1] triangle wave
			float triangleWave;
			if (totalPhase < 0.5f)
				triangleWave = 4.0f * totalPhase - 1.0f; // Rising edge: [0,0.5] -> [-1,1]
			else
				triangleWave = 3.0f - 4.0f * totalPhase; // Falling edge: [0.5,1] -> [1,-1]
			
			out_Value = amplitude * triangleWave;
		}

	private:
		float m_Phase{ 0.0f };
	};

	//==============================================================================
	// Noise Generator - Multiple noise types
	//==============================================================================
	struct Noise : public NodeProcessor
	{
		explicit Noise(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{ 
			RegisterEndpoints();
		}

		// Input parameters
		int* in_Seed = nullptr;
		int32_t* in_Type = nullptr;			// Noise type (0=White, 1=Pink, 2=Brown)
		float* in_Amplitude = nullptr;		// Output amplitude

		// Output
		float out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();

			// Initialize with safe input resolution
			int resolvedSeed = ResolveSeed();
			ENoiseType resolvedType = ResolveType();
			
			// Cache the resolved values
			m_CachedSeed = resolvedSeed;
			m_CachedType = resolvedType;
			
			// Initialize generator
			m_Generator.Init(resolvedSeed, resolvedType);
		}

		void Process() final
		{
			// Check if seed or type have changed and reinitialize if needed
			int resolvedSeed = ResolveSeed();
			ENoiseType resolvedType = ResolveType();
			
			if (resolvedSeed != m_CachedSeed || resolvedType != m_CachedType)
			{
				m_CachedSeed = resolvedSeed;
				m_CachedType = resolvedType;
				m_Generator.Init(resolvedSeed, resolvedType);
			}
			
			float noiseValue = m_Generator.GetNextValue();
			float amplitude = in_Amplitude ? *in_Amplitude : 1.0f;
			out_Value = noiseValue * amplitude;
		}

		enum ENoiseType : int32_t
		{
			WhiteNoise = 0, 
			PinkNoise = 1, 
			BrownNoise = 2
		};

	private:
		// Helper methods for safe input resolution
		int ResolveSeed() const
		{
			return (in_Seed && *in_Seed != -1) ? *in_Seed : static_cast<int>(std::time(nullptr));
		}
		
		ENoiseType ResolveType() const
		{
			return (in_Type) ? static_cast<ENoiseType>(*in_Type) : WhiteNoise;
		}
		
		// Cached values to detect changes
		int m_CachedSeed = -1;
		ENoiseType m_CachedType = WhiteNoise;

		struct Generator
		{
		public:
			void Init(int32_t seed, ENoiseType noiseType)
			{
				m_Type = noiseType;
				SetSeed(seed);

				if (m_Type == ENoiseType::PinkNoise)
				{
					memset(m_PinkState.bins, 0, sizeof(m_PinkState.bins));
					m_PinkState.accumulation = 0.0f;
					m_PinkState.counter = 1;
				}

				if (m_Type == ENoiseType::BrownNoise)
				{
					m_BrownState.accumulation = 0.0f;
				}
			}

			void SetSeed(int32_t seed) noexcept
			{
				m_Random.SetSeed(seed);
			}

			float GetNextValue()
			{
				switch (m_Type)
				{
					case WhiteNoise: 	return GetNextValueWhite();
					case PinkNoise: 	return GetNextValuePink();
					case BrownNoise: 	return GetNextValueBrown();
					default: 			return GetNextValueWhite();
				}
			}

		private:
			float GetNextValueWhite()
			{
				return m_Random.GetFloat32InRange(-1.0f, 1.0f);
			}

			float GetNextValuePink()
			{
				// Paul Kellet's refined pink noise algorithm
				float white = m_Random.GetFloat32InRange(-1.0f, 1.0f);
				
				m_PinkState.bins[0] = 0.99886f * m_PinkState.bins[0] + white * 0.0555179f;
				m_PinkState.bins[1] = 0.99332f * m_PinkState.bins[1] + white * 0.0750759f;
				m_PinkState.bins[2] = 0.96900f * m_PinkState.bins[2] + white * 0.1538520f;
				m_PinkState.bins[3] = 0.86650f * m_PinkState.bins[3] + white * 0.3104856f;
				m_PinkState.bins[4] = 0.55000f * m_PinkState.bins[4] + white * 0.5329522f;
				m_PinkState.bins[5] = -0.7616f * m_PinkState.bins[5] - white * 0.0168980f;
				
				float pink = m_PinkState.bins[0] + m_PinkState.bins[1] + m_PinkState.bins[2] + 
							m_PinkState.bins[3] + m_PinkState.bins[4] + m_PinkState.bins[5] + 
							m_PinkState.bins[6] + white * 0.5362f;
				
				m_PinkState.bins[6] = white * 0.115926f;
				
				return glm::clamp(pink * 0.11f, -1.0f, 1.0f); // Scale and clamp
			}

			float GetNextValueBrown()
			{
				// Brownian noise (red noise) - integrated white noise
				float white = m_Random.GetFloat32InRange(-1.0f, 1.0f);
				m_BrownState.accumulation += white * 0.02f; // Integration step
				
				// Prevent DC drift
				m_BrownState.accumulation *= 0.9999f;
				
				// Clamp to prevent overflow
				m_BrownState.accumulation = glm::clamp(m_BrownState.accumulation, -1.0f, 1.0f);
				
				return m_BrownState.accumulation;
			}

			ENoiseType m_Type{ WhiteNoise };
			FastRandom m_Random;

			// Pink noise state
			struct {
				float bins[7]{ 0.0f };
				float accumulation{ 0.0f };
				uint32_t counter{ 1 };
			} m_PinkState;

			// Brown noise state
			struct {
				float accumulation{ 0.0f };
			} m_BrownState;
		};

		Generator m_Generator;
	};
}

#undef DECLARE_ID