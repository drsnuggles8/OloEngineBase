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

        // Input parameters — audio-rate capable (FM / AM when wired from another
        // audio producer; scalar broadcast when set from a parameter or default).
        AudioBufferRef m_Frequency; // Frequency in Hz
        AudioBufferRef m_Amplitude; // Amplitude (0.0 to 1.0)
        AudioBufferRef m_Phase;     // Phase offset in radians

        // Output
        AudioBuffer m_OutValue;

        void RegisterEndpoints();

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            m_PhaseAccumulator = 0.0;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            f32* out = m_OutValue.Data();

            // Guard against zero or near-zero sample rate (block-rate check)
            if (m_SampleRate <= 1e-6f)
            {
                std::fill_n(out, numFrames, 0.0f);
                return;
            }

            const f64 invSampleRate = 1.0 / static_cast<f64>(m_SampleRate);

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                const f32 frequency = glm::max(0.0f, m_Frequency.Sample(frame));
                const f32 amplitude = glm::clamp(m_Amplitude.Sample(frame), 0.0f, 1.0f);
                const f32 phaseOffset = m_Phase.Sample(frame);

                m_PhaseAccumulator += static_cast<f64>(frequency) * invSampleRate;
                m_PhaseAccumulator -= std::floor(m_PhaseAccumulator); // wrap to [0, 1)

                f32 totalPhase = static_cast<f32>(m_PhaseAccumulator) + (phaseOffset / (2.0f * std::numbers::pi_v<f32>));
                totalPhase = totalPhase - std::floor(totalPhase);

                out[frame] = amplitude * std::sin(2.0f * std::numbers::pi_v<f32> * totalPhase);
            }
        }

      private:
        f64 m_PhaseAccumulator{ 0.0 };
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
        AudioBufferRef m_Frequency;  // Frequency in Hz
        AudioBufferRef m_Amplitude;  // Amplitude (0.0 to 1.0)
        AudioBufferRef m_Phase;      // Phase offset in radians
        AudioBufferRef m_PulseWidth; // Pulse width (0.0 to 1.0, 0.5 = square)

        // Output
        AudioBuffer m_OutValue;

        void RegisterEndpoints();

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            m_PhaseAccumulator = 0.0;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            f32* out = m_OutValue.Data();

            if (m_SampleRate <= 1e-6f)
            {
                std::fill_n(out, numFrames, 0.0f);
                return;
            }

            const f64 invSampleRate = 1.0 / static_cast<f64>(m_SampleRate);

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                const f32 frequency = glm::max(0.0f, m_Frequency.Sample(frame));
                const f32 amplitude = glm::clamp(m_Amplitude.Sample(frame), 0.0f, 1.0f);
                const f32 phaseOffset = m_Phase.Sample(frame);
                const f32 pulseWidth = glm::clamp(m_PulseWidth.Sample(frame), 0.01f, 0.99f);

                m_PhaseAccumulator += static_cast<f64>(frequency) * invSampleRate;
                m_PhaseAccumulator -= std::floor(m_PhaseAccumulator);

                f32 totalPhase = static_cast<f32>(m_PhaseAccumulator) + (phaseOffset / (2.0f * std::numbers::pi_v<f32>));
                totalPhase = std::fmod(totalPhase, 1.0f);
                if (totalPhase < 0.0f)
                    totalPhase += 1.0f;

                out[frame] = amplitude * (totalPhase < pulseWidth ? 1.0f : -1.0f);
            }
        }

      private:
        f64 m_PhaseAccumulator{ 0.0 };
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
        AudioBufferRef m_Frequency; // Frequency in Hz
        AudioBufferRef m_Amplitude; // Amplitude (0.0 to 1.0)
        AudioBufferRef m_Phase;     // Phase offset in radians

        // Output
        AudioBuffer m_OutValue;

        void RegisterEndpoints();

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            m_PhaseAccumulator = 0.0;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            f32* out = m_OutValue.Data();

            if (m_SampleRate <= 1e-6f)
            {
                std::fill_n(out, numFrames, 0.0f);
                return;
            }

            const f64 invSampleRate = 1.0 / static_cast<f64>(m_SampleRate);

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                const f32 frequency = glm::max(0.0f, m_Frequency.Sample(frame));
                const f32 amplitude = glm::clamp(m_Amplitude.Sample(frame), 0.0f, 1.0f);
                const f32 phaseOffset = m_Phase.Sample(frame);

                m_PhaseAccumulator += static_cast<f64>(frequency) * invSampleRate;
                m_PhaseAccumulator = m_PhaseAccumulator - std::floor(m_PhaseAccumulator);

                f32 totalPhase = static_cast<f32>(m_PhaseAccumulator) + (phaseOffset / (2.0f * std::numbers::pi_v<f32>));
                totalPhase = std::fmod(totalPhase + 1.0f, 1.0f);

                out[frame] = amplitude * (2.0f * totalPhase - 1.0f);
            }
        }

      private:
        f64 m_PhaseAccumulator{ 0.0 };
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
        AudioBufferRef m_Frequency; // Frequency in Hz
        AudioBufferRef m_Amplitude; // Amplitude (0.0 to 1.0)
        AudioBufferRef m_Phase;     // Phase offset in radians

        // Output
        AudioBuffer m_OutValue;

        void RegisterEndpoints();

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            m_PhaseAccumulator = 0.0;
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            f32* out = m_OutValue.Data();

            if (m_SampleRate <= 1e-6f)
            {
                std::fill_n(out, numFrames, 0.0f);
                return;
            }

            const f64 invSampleRate = 1.0 / static_cast<f64>(m_SampleRate);

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                const f32 frequency = glm::max(0.0f, m_Frequency.Sample(frame));
                const f32 amplitude = glm::clamp(m_Amplitude.Sample(frame), 0.0f, 1.0f);
                const f32 phaseOffset = m_Phase.Sample(frame);

                m_PhaseAccumulator += static_cast<f64>(frequency) * invSampleRate;
                if (m_PhaseAccumulator >= 1.0)
                    m_PhaseAccumulator = std::fmod(m_PhaseAccumulator, 1.0);

                f32 totalPhase = static_cast<f32>(m_PhaseAccumulator) + (phaseOffset / (2.0f * std::numbers::pi_v<f32>));
                totalPhase = std::fmod(totalPhase + 1.0f, 1.0f);

                f32 triangleWave;
                if (totalPhase < 0.5f)
                    triangleWave = 4.0f * totalPhase - 1.0f;
                else
                    triangleWave = 3.0f - 4.0f * totalPhase;

                out[frame] = amplitude * triangleWave;
            }
        }

      private:
        f64 m_PhaseAccumulator{ 0.0 };
    };

    //==============================================================================
    // Noise Generator - Multiple noise types
    //==============================================================================
    struct Noise : public NodeProcessor
    {
        explicit Noise(const char* dbgName, UUID id)
            : NodeProcessor(dbgName, id)
        {
            RegisterEndpoints();
        }

        // Input parameters
        IntRef m_Seed;
        IntRef m_Type;              // Noise type (0=White, 1=Pink, 2=Brown)
        AudioBufferRef m_Amplitude; // Output amplitude

        // Output
        AudioBuffer m_OutValue;

        void RegisterEndpoints();

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            // Initialize fallback seed with high-entropy construction
            static std::atomic<u64> s_Counter{ 0 };

            // Combine multiple entropy sources
            u64 counter = s_Counter.fetch_add(1, std::memory_order_relaxed);
            u64 timestamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            u64 randomDevice = 0;
            try
            {
                std::random_device rd;
                randomDevice = static_cast<u64>(rd()) << 32 | rd();
            }
            catch (...)
            {
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
            i32 resolvedSeed = ResolveSeed();
            ENoiseType resolvedType = ResolveType();

            // Cache the resolved values
            m_CachedSeed = resolvedSeed;
            m_CachedType = resolvedType;

            // Initialize generator
            m_Generator.Init(resolvedSeed, resolvedType);
        }

        void Process(u32 numFrames) final
        {
            OLO_PROFILE_FUNCTION();

            // Check if seed or type have changed and reinitialize if needed.
            // Seed/type are control-rate — stable across the block.
            const i32 resolvedSeed = ResolveSeed();
            const ENoiseType resolvedType = ResolveType();

            if (resolvedSeed != m_CachedSeed || resolvedType != m_CachedType)
            {
                m_CachedSeed = resolvedSeed;
                m_CachedType = resolvedType;
                m_Generator.Init(resolvedSeed, resolvedType);
            }

            f32* out = m_OutValue.Data();
            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                out[frame] = m_Generator.GetNextValue() * m_Amplitude.Sample(frame);
            }
        }

        enum ENoiseType : int32_t
        {
            WhiteNoise = 0,
            PinkNoise = 1,
            BrownNoise = 2
        };

      private:
        // Helper methods for safe input resolution
        i32 ResolveSeed() const
        {
            OLO_PROFILE_FUNCTION();

            if (const i32 seed = m_Seed.Get(); seed != -1)
                return seed;

            // Use pre-initialized fallback seed (thread-safe)
            return m_FallbackSeed;
        }

        ENoiseType ResolveType() const
        {
            OLO_PROFILE_FUNCTION();

            return static_cast<ENoiseType>(m_Type.Get());
        }

        // Cached values to detect changes
        i32 m_CachedSeed = -1;
        ENoiseType m_CachedType = WhiteNoise;

        // Pre-initialized fallback seed for when input seed is unset (-1) - thread-safe
        std::atomic<i32> m_FallbackSeed{ 0 };

        struct Generator
        {
          public:
            void Init(i32 seed, ENoiseType noiseType)
            {
                OLO_PROFILE_FUNCTION();

                m_Type = noiseType;
                SetSeed(seed);

                if (m_Type == ENoiseType::PinkNoise)
                {
                    // bins are already zero-initialized by member initializer (line 509)
                    m_PinkState.m_Accumulation = 0.0f;
                    m_PinkState.m_Counter = 1;
                }

                if (m_Type == ENoiseType::BrownNoise)
                {
                    m_BrownState.m_Accumulation = 0.0f;
                }
            }

            void SetSeed(i32 seed) noexcept
            {
                OLO_PROFILE_FUNCTION();

                m_Random.SetSeed(seed);
            }

            f32 GetNextValue()
            {
                // Intentionally no OLO_PROFILE_FUNCTION: called once per audio frame.
                switch (m_Type)
                {
                    case WhiteNoise:
                        return GetNextValueWhite();
                    case PinkNoise:
                        return GetNextValuePink();
                    case BrownNoise:
                        return GetNextValueBrown();
                    default:
                        return GetNextValueWhite();
                }
            }

          private:
            f32 GetNextValueWhite()
            {
                return m_Random.GetFloat32InRange(-1.0f, 1.0f);
            }

            f32 GetNextValuePink()
            {
                // Paul Kellet's refined pink noise algorithm
                f32 white = m_Random.GetFloat32InRange(-1.0f, 1.0f);

                m_PinkState.m_Bins[0] = 0.99886f * m_PinkState.m_Bins[0] + white * 0.0555179f;
                m_PinkState.m_Bins[1] = 0.99332f * m_PinkState.m_Bins[1] + white * 0.0750759f;
                m_PinkState.m_Bins[2] = 0.96900f * m_PinkState.m_Bins[2] + white * 0.1538520f;
                m_PinkState.m_Bins[3] = 0.86650f * m_PinkState.m_Bins[3] + white * 0.3104856f;
                m_PinkState.m_Bins[4] = 0.55000f * m_PinkState.m_Bins[4] + white * 0.5329522f;
                m_PinkState.m_Bins[5] = -0.7616f * m_PinkState.m_Bins[5] - white * 0.0168980f;

                f32 pink = m_PinkState.m_Bins[0] + m_PinkState.m_Bins[1] + m_PinkState.m_Bins[2] +
                           m_PinkState.m_Bins[3] + m_PinkState.m_Bins[4] + m_PinkState.m_Bins[5] +
                           m_PinkState.m_Bins[6] + white * 0.5362f;

                m_PinkState.m_Bins[6] = white * 0.115926f;

                return glm::clamp(pink * 0.11f, -1.0f, 1.0f); // Scale and clamp
            }

            f32 GetNextValueBrown()
            {
                // Brownian noise (red noise) - integrated white noise
                f32 white = m_Random.GetFloat32InRange(-1.0f, 1.0f);
                m_BrownState.m_Accumulation += white * 0.02f; // Integration step

                // Prevent DC drift
                m_BrownState.m_Accumulation *= 0.9999f;

                // Clamp to prevent overflow
                m_BrownState.m_Accumulation = glm::clamp(m_BrownState.m_Accumulation, -1.0f, 1.0f);

                return m_BrownState.m_Accumulation;
            }

            ENoiseType m_Type{ WhiteNoise };
            FastRandomPCG m_Random;

            // Pink noise state
            struct
            {
                f32 m_Bins[7]{ 0.0f };
                f32 m_Accumulation{ 0.0f };
                u32 m_Counter{ 1 };
            } m_PinkState;

            // Brown noise state
            struct
            {
                f32 m_Accumulation{ 0.0f };
            } m_BrownState;
        };

        Generator m_Generator;
    };
} // namespace OloEngine::Audio::SoundGraph
