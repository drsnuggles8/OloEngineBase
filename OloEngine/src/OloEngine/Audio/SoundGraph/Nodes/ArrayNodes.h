#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/FastRandom.h"

#include <vector>
#include <chrono>
#include <type_traits>
#include <algorithm>

#include <glm/glm.hpp>

#define DECLARE_ID(name)                 \
    static constexpr Identifier s_##name \
    {                                    \
        #name                            \
    }

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// Detail namespace for internal helper functions
    namespace Detail
    {
        /// Helper function to resolve a seed input value.
        /// Returns: high-resolution clock value if seed == -1 (the "unseeded"
        /// sentinel), otherwise the provided seed.
        inline i32 GetRandomSeedValue(i32 seed)
        {
            return (seed == -1)
                       ? static_cast<i32>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
                       : seed;
        }
    } // namespace Detail

    //==============================================================================
    /// GetRandom - Get random item from an array
    //==============================================================================
    template<typename T>
    struct GetRandom : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(Next);
            DECLARE_ID(Reset);

          private:
            IDs() = delete;
        };

        explicit GetRandom(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            AddInEvent(IDs::s_Next, [this](float v)
                       { (void)v; m_NextFlag.SetDirty(); });
            AddInEvent(IDs::s_Reset, [this](float v)
                       { (void)v; m_ResetFlag.SetDirty(); });

            RegisterEndpoints();

            // Unwired seed defaults to the -1 "unseeded" sentinel so
            // GetSeedValue() derives a clock seed — a 0 default would make every
            // unconfigured instance produce the same sequence.
            m_Seed.SetDefault(-1);
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            // Initialize random generator with seed
            m_Random.SetSeed(GetSeedValue());

            m_OutElement = T{};
        }

        void Process(u32 numFrames) final
        {
            (void)numFrames; // Event-driven array node; trigger flags drive output, not block size.
            OLO_PROFILE_FUNCTION();

            if (m_NextFlag.CheckAndResetIfDirty())
                ProcessNext();

            if (m_ResetFlag.CheckAndResetIfDirty())
                ProcessReset();
        }

        //==========================================================================
        /// NodeProcessor setup
        // Array plugs aren't expressible as typed stream connections (no primitive
        // type); they stay raw pointers and the node null-checks them.
        std::vector<T>* m_Array = nullptr;
        IntRef m_Min;
        IntRef m_Max;
        IntRef m_Seed;

        OutputEvent m_OnNext{ *this };
        OutputEvent m_OnReset{ *this };
        T m_OutElement{ T{} };

      private:
        Flag m_NextFlag;
        Flag m_ResetFlag;
        FastRandomPCG m_Random;

        void RegisterEndpoints();

        /// Helper method to resolve the seed: -1 (default) means clock-derived.
        i32 GetSeedValue() const
        {
            return Detail::GetRandomSeedValue(m_Seed.Get());
        }

        void ProcessNext()
        {
            // Validate array exists and is not empty
            if (!m_Array || m_Array->size() == 0)
            {
                // Handle null/empty array gracefully - set default value and return
                m_OutElement = T{};
                OLO_CORE_WARN("GetRandom: Array is null or empty, using default value");
                return;
            }

            // Compute valid index range within array bounds
            i32 arraySize = static_cast<i32>(m_Array->size());

            // Clamp both bounds to valid array indices [0, arraySize-1]
            i32 minIndex = std::clamp(m_Min.Get(), 0, arraySize - 1);
            i32 maxIndex = std::clamp(m_Max.Get(), 0, arraySize - 1);

            // Ensure minIndex <= maxIndex by swapping if necessary
            if (minIndex > maxIndex)
            {
                std::swap(minIndex, maxIndex);
            }

            // Generate random index within valid bounds
            i32 randomIndex = m_Random.GetInt32InRange(minIndex, maxIndex);

            // Validate index is within bounds (safety check)
            if (randomIndex >= 0 && randomIndex < arraySize)
            {
                m_OutElement = (*m_Array)[randomIndex];
                m_OnNext(1.0f);
            }
            else
            {
                // Out-of-range fallback - should not happen with proper bounds checking
                m_OutElement = T{};
                OLO_CORE_ERROR("GetRandom: Generated index {} out of bounds [0, {})", randomIndex, arraySize);
            }
        }

        void ProcessReset()
        {
            // Reset random generator with seed
            m_Random.SetSeed(GetSeedValue());
            m_OnReset(1.0f);
        }
    };

    //==============================================================================
    /// Get - Get item from an array by index
    //==============================================================================
    template<typename T>
    struct Get : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(Trigger);

          private:
            IDs() = delete;
        };

        explicit Get(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            OLO_PROFILE_FUNCTION();

            AddInEvent(IDs::s_Trigger, [this](float v)
                       { (void)v; m_TriggerFlag.SetDirty(); });

            RegisterEndpoints();
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            m_OutElement = T{};
        }

        void Process(u32 numFrames) final
        {
            (void)numFrames; // Event-driven array node; trigger flags drive output, not block size.
            OLO_PROFILE_FUNCTION();

            if (m_TriggerFlag.CheckAndResetIfDirty())
                ProcessTrigger();
        }

        //==========================================================================
        /// NodeProcessor setup
        // Array plugs aren't expressible as typed stream connections; see GetRandom.
        std::vector<T>* m_Array = nullptr;
        IntRef m_Index;

        OutputEvent m_OnTrigger{ *this };
        T m_OutElement{ T{} };

      private:
        Flag m_TriggerFlag;

        void RegisterEndpoints();

        void TriggerOutput(const T& element)
        {
            m_OutElement = element;
            if constexpr (std::is_arithmetic_v<T>)
                m_OnTrigger(static_cast<f32>(m_OutElement));
            else
                m_OnTrigger(1.0f);
        }

        void ProcessTrigger()
        {
            // Validate array exists and is not empty
            if (!m_Array || m_Array->size() == 0)
            {
                // Handle null/empty array gracefully - set default value and return
                m_OutElement = T{};
                OLO_CORE_WARN("ArrayGet: Array is null or empty, using default value");
                return;
            }

            // Perform bounds checking against actual array size
            i32 index = m_Index.Get();
            i32 arraySize = static_cast<i32>(m_Array->size());

            if (index >= 0 && index < arraySize)
            {
                // Valid index - get element from array
                TriggerOutput((*m_Array)[index]);
            }
            else
            {
                // Index out of bounds - output default and warn
                TriggerOutput(T{});
                OLO_CORE_WARN("ArrayGet: Index {} out of bounds for array of size {}", index, arraySize);
            }
        }
    };

    //==============================================================================
    /// Random - Generate random values (simplified version for OloEngine)
    //==============================================================================
    template<typename T>
    struct Random : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(Next);
            DECLARE_ID(Reset);

          private:
            IDs() = delete;
        };

        explicit Random(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            OLO_PROFILE_FUNCTION();
            AddInEvent(IDs::s_Next, [this](float v)
                       { (void)v; m_NextFlag.SetDirty(); });
            AddInEvent(IDs::s_Reset, [this](float v)
                       { (void)v; m_ResetFlag.SetDirty(); });

            RegisterEndpoints();

            // Unwired seed defaults to the -1 "unseeded" sentinel so
            // GetSeedValue() derives a clock seed — a 0 default would make every
            // unconfigured instance produce the same sequence.
            m_Seed.SetDefault(-1);
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();

            // Initialize random generator with seed
            m_Random.SetSeed(GetSeedValue());

            m_OutValue = T{};
        }

        void Process(u32 numFrames) final
        {
            (void)numFrames; // Event-driven array node; trigger flags drive output, not block size.
            OLO_PROFILE_FUNCTION();

            if (m_NextFlag.CheckAndResetIfDirty())
                ProcessNext();

            if (m_ResetFlag.CheckAndResetIfDirty())
                ProcessReset();
        }

        //==========================================================================
        /// NodeProcessor setup
        ValueRef<T> m_Min;
        ValueRef<T> m_Max;
        IntRef m_Seed;

        OutputEvent m_OnNext{ *this };
        OutputEvent m_OnReset{ *this };
        T m_OutValue{ T{} };

      private:
        Flag m_NextFlag;
        Flag m_ResetFlag;
        FastRandomPCG m_Random;

        void RegisterEndpoints();

        /// Helper method to resolve the seed: -1 (default) means clock-derived.
        i32 GetSeedValue() const
        {
            return Detail::GetRandomSeedValue(m_Seed.Get());
        }

        void ProcessNext()
        {
            T randomValue;

            // Get type-appropriate bounds and normalize min/max ordering
            T minValue, maxValue;

            if constexpr (std::is_same_v<T, f32>)
            {
                // For f32, clamp to reasonable audio range
                minValue = glm::clamp(m_Min.Get(), static_cast<f32>(-1000.0), static_cast<f32>(1000.0));
                maxValue = glm::clamp(m_Max.Get(), static_cast<f32>(-1000.0), static_cast<f32>(1000.0));

                // Ensure min <= max
                if (minValue > maxValue)
                    std::swap(minValue, maxValue);

                randomValue = m_Random.GetFloat32InRange(minValue, maxValue);
            }
            else if constexpr (std::is_integral_v<T>)
            {
                // For integers, clamp to reasonable range
                constexpr T typeMin = static_cast<T>(-100000);
                constexpr T typeMax = static_cast<T>(100000);
                minValue = glm::clamp(m_Min.Get(), typeMin, typeMax);
                maxValue = glm::clamp(m_Max.Get(), typeMin, typeMax);

                // Ensure min <= max
                if (minValue > maxValue)
                    std::swap(minValue, maxValue);

                randomValue = static_cast<T>(m_Random.GetInt32InRange(static_cast<i32>(minValue), static_cast<i32>(maxValue)));
            }
            else
            {
                randomValue = T{};
            }

            m_OutValue = randomValue;
            m_OnNext(1.0f);
        }

        void ProcessReset()
        {
            // Reset random generator with seed
            m_Random.SetSeed(GetSeedValue());
            m_OnReset(1.0f);
        }
    };

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
