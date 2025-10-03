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
#include <optional>
#include <atomic>
#include <chrono>
#include <random>

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
		f32* in_Frequency = nullptr;		// Frequency in Hz
		f32* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		f32* in_Phase = nullptr;			// Phase offset in radians

		// Output
		f32 out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			OLO_PROFILE_FUNCTION();
			
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0f;
		}

		void Process() final
		{
			OLO_PROFILE_FUNCTION();
			
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;

			// Guard against zero or near-zero sample rate
			if (m_SampleRate <= 1e-6f)
			{
				// Return silence for invalid sample rate
				out_Value = 0.0f;
				return;
			}

			// Update phase using double precision for higher accuracy
			double deltaPhase = static_cast<double>(frequency) / static_cast<double>(m_SampleRate);
			m_Phase += deltaPhase;
			
			// Robust phase wrapping to keep in [0, 1) - handles negative values correctly
			m_Phase -= std::floor(m_Phase);
			
			// Calculate sine with phase offset
			float totalPhase = static_cast<float>(m_Phase) + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			// Robust wrap to [0, 1) - handles large negative offsets correctly
			totalPhase = totalPhase - std::floor(totalPhase);
			
		out_Value = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * totalPhase);
	}

private:
	double m_Phase{ 0.0 };
};	//==============================================================================
	// Square Wave Oscillator  
	//==============================================================================
	struct SquareOscillator : public NodeProcessor
	{
		explicit SquareOscillator(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		// Input parameters
		f32* in_Frequency = nullptr;		// Frequency in Hz
		f32* in_Amplitude = nullptr;		// Amplitude (0.0 to 1.0)
		f32* in_Phase = nullptr;			// Phase offset in radians
		f32* in_PulseWidth = nullptr;		// Pulse width (0.0 to 1.0, 0.5 = square)

		// Output
		f32 out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			OLO_PROFILE_FUNCTION();
			
			InitializeInputs();
			// Sample rate is now set by NodeProcessor base class
			m_Phase = 0.0;
		}

		void Process() final
		{
			OLO_PROFILE_FUNCTION();
			
			float frequency = glm::max(0.0f, *in_Frequency);
			float amplitude = glm::clamp(*in_Amplitude, 0.0f, 1.0f);
			float phaseOffset = *in_Phase;
			float pulseWidth = glm::clamp(*in_PulseWidth, 0.01f, 0.99f);

			// Guard against zero or near-zero sample rate
			if (m_SampleRate <= 1e-6f)
			{
				// Return silence for invalid sample rate
				out_Value = 0.0f;
				return;
			}

			// Update phase using double precision for higher accuracy
			double deltaPhase = static_cast<double>(frequency) / static_cast<double>(m_SampleRate);
			m_Phase += deltaPhase;
			
			// Robust phase wrapping to keep in [0, 1) - handles negative values correctly
			m_Phase -= std::floor(m_Phase);
			
			// Calculate square with phase offset and pulse width
			float totalPhase = static_cast<float>(m_Phase) + (phaseOffset / (2.0f * std::numbers::pi_v<float>));
			// Robust wrap to [0, 1) - handles large negative offsets correctly
			totalPhase = std::fmod(totalPhase, 1.0f);
			if (totalPhase < 0.0f)
				totalPhase += 1.0f;
			
			out_Value = amplitude * (totalPhase < pulseWidth ? 1.0f : -1.0f);
		}

	private:
		double m_Phase{ 0.0 };
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

			// Guard against zero or near-zero sample rate
			if (m_SampleRate <= 1e-6f)
			{
				// Return silence for invalid sample rate
				out_Value = 0.0f;
				return;
			}

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

			// Guard against zero or near-zero sample rate
			if (m_SampleRate <= 1e-6f)
			{
				// Return silence for invalid sample rate
				out_Value = 0.0f;
				return;
			}

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
		i32* in_Seed = nullptr;
		i32* in_Type = nullptr;			// Noise type (0=White, 1=Pink, 2=Brown)
		f32* in_Amplitude = nullptr;		// Output amplitude

		// Output
		f32 out_Value{ 0.0f };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			OLO_PROFILE_FUNCTION();
			
			InitializeInputs();

			// Initialize fallback seed with high-entropy construction
			static std::atomic<u64> s_Counter{ 0 };
			
			// Combine multiple entropy sources
			u64 counter = s_Counter.fetch_add(1, std::memory_order_relaxed);
			u64 timestamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			u64 randomDevice = 0;
			try {
				std::random_device rd;
				randomDevice = static_cast<u64>(rd()) << 32 | rd();
			} catch (...) {
				// Fallback if random_device fails
				randomDevice = static_cast<u64>(std::time(nullptr));
			}
			u64 nodeAddress = reinterpret_cast<uintptr_t>(this);
			
			// Mix entropy sources using simple hash combining
			u64 seed64 = counter;
			seed64 ^= timestamp + 0x9e3779b9 + (seed64 << 6) + (seed64 >> 2);
			seed64 ^= randomDevice + 0x9e3779b9 + (seed64 << 6) + (seed64 >> 2);
			seed64 ^= nodeAddress + 0x9e3779b9 + (seed64 << 6) + (seed64 >> 2);
			
			// Deterministic narrowing to int
			m_FallbackSeed = static_cast<int>(seed64 ^ (seed64 >> 32));

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
			OLO_PROFILE_FUNCTION();
			
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
			if (in_Seed && *in_Seed != -1)
			{
				return *in_Seed;
			}
			else
			{
				// Use pre-initialized fallback seed (thread-safe)
				return m_FallbackSeed;
			}
		}
		
		ENoiseType ResolveType() const
		{
			return (in_Type) ? static_cast<ENoiseType>(*in_Type) : WhiteNoise;
		}
		
		// Cached values to detect changes
		int m_CachedSeed = -1;
		ENoiseType m_CachedType = WhiteNoise;
		
		// Pre-initialized fallback seed for when input seed is unset (-1) - thread-safe
		std::atomic<int> m_FallbackSeed{0};

		struct Generator
		{
		public:
			void Init(int32_t seed, ENoiseType noiseType)
			{
				m_Type = noiseType;
				SetSeed(seed);

				if (m_Type == ENoiseType::PinkNoise)
				{
					// bins are already zero-initialized by member initializer (line 478)
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
				f32 bins[7]{ 0.0f };
				f32 accumulation{ 0.0f };
				u32 counter{ 1 };
			} m_PinkState;

			// Brown noise state
			struct {
				f32 accumulation{ 0.0f };
			} m_BrownState;
		};

		Generator m_Generator;
	};
}

#undef DECLARE_ID