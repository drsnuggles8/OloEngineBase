#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/FastRandom.h"

#include <vector>
#include <chrono>
#include <type_traits>
#include <algorithm>

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
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
			AddInEvent(IDs::Next, [this](float v) { m_NextFlag.SetDirty(); });
			AddInEvent(IDs::Reset, [this](float v) { m_ResetFlag.SetDirty(); });
			
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();

			// Initialize random generator with safe null checking
			// If in_Seed is null (initialization failed), use high-resolution clock as fallback
			// If in_Seed is -1, use high-resolution clock for truly random seed
			// Otherwise use the provided seed value
			int32_t seed;
			if (!in_Seed) {
				// Fallback: use high-resolution clock when in_Seed is null
				seed = static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
			} else {
				seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			}
			m_Random.SetSeed(seed);
			
			out_Element = T{};
		}

		void Process() final
		{
			if (m_NextFlag.CheckAndResetIfDirty())
				ProcessNext();

			if (m_ResetFlag.CheckAndResetIfDirty())
				ProcessReset();
		}

		//==========================================================================
		/// NodeProcessor setup
		std::vector<T>* in_Array = nullptr;
		int32_t* in_Min = nullptr;
		int32_t* in_Max = nullptr;
		int32_t* in_Seed = nullptr;

		OutputEvent out_OnNext{ *this };
		OutputEvent out_OnReset{ *this };
		T out_Element{ T{} };

	private:
		Flag m_NextFlag;
		Flag m_ResetFlag;
		FastRandom m_Random;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessNext()
		{
			// Validate array exists and is not empty
			if (!in_Array || in_Array->size() == 0)
			{
				// Handle null/empty array gracefully - set default value and return
				out_Element = T{};
				OLO_CORE_WARN("GetRandom: Array is null or empty, using default value");
				return;
			}

			// Compute valid index range within array bounds
			int32_t arraySize = static_cast<int32_t>(in_Array->size());
			
			// Handle empty array case
			if (arraySize == 0)
			{
				out_Element = T{};
				OLO_CORE_WARN("GetRandom: Array is empty, using default value");
				return;
			}
			
			// Clamp both bounds to valid array indices [0, arraySize-1]
			int32_t minIndex = (in_Min) ? std::clamp(*in_Min, 0, arraySize - 1) : 0;
			int32_t maxIndex = (in_Max) ? std::clamp(*in_Max, 0, arraySize - 1) : arraySize - 1;
			
			// Ensure minIndex <= maxIndex by swapping if necessary
			if (minIndex > maxIndex)
			{
				std::swap(minIndex, maxIndex);
			}

			// Generate random index within valid bounds
			int32_t randomIndex = m_Random.GetInt32InRange(minIndex, maxIndex);
			
			// Validate index is within bounds (safety check)
			if (randomIndex >= 0 && randomIndex < arraySize)
			{
				out_Element = (*in_Array)[randomIndex];
				out_OnNext(1.0f);
			}
			else
			{
				// Out-of-range fallback - should not happen with proper bounds checking
				out_Element = T{};
				OLO_CORE_ERROR("GetRandom: Generated index {} out of bounds [0, {})", randomIndex, arraySize);
			}
		}

		void ProcessReset()
		{
			// Safe seed handling with null checking
			// If in_Seed is null (initialization failed), use high-resolution clock as fallback
			// If in_Seed is -1, use high-resolution clock for truly random seed
			// Otherwise use the provided seed value
			int32_t seed;
			if (!in_Seed) {
				// Fallback: use high-resolution clock when in_Seed is null
				seed = static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
			} else {
				seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			}
			m_Random.SetSeed(seed);
			out_OnReset(1.0f);
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
			AddInEvent(IDs::Trigger, [this](float v) { m_TriggerFlag.SetDirty(); });
			
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();
			out_Element = T{};
		}

		void Process() final
		{
			if (m_TriggerFlag.CheckAndResetIfDirty())
				ProcessTrigger();
		}

		//==========================================================================
		/// NodeProcessor setup
		std::vector<T>* in_Array = nullptr;
		int32_t* in_Index = nullptr;

		OutputEvent out_OnTrigger{ *this };
		T out_Element{ T{} };

	private:
		Flag m_TriggerFlag;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessTrigger()
		{
			// Validate array exists and is not empty
			if (!in_Array || in_Array->size() == 0)
			{
				// Handle null/empty array gracefully - set default value and return
				out_Element = T{};
				OLO_CORE_WARN("ArrayGet: Array is null or empty, using default value");
				return;
			}

			// Validate index pointer exists
			if (!in_Index)
			{
				// No index provided - use first element (index 0)
				out_Element = (*in_Array)[0];
				if constexpr (std::is_arithmetic_v<T>)
					out_OnTrigger(static_cast<float>(out_Element));
				else
					out_OnTrigger(1.0f);
				return;
			}

			// Perform bounds checking against actual array size
			int32_t index = *in_Index;
			int32_t arraySize = static_cast<int32_t>(in_Array->size());
			
			if (index >= 0 && index < arraySize)
			{
				// Valid index - get element from array
				out_Element = (*in_Array)[index];
				if constexpr (std::is_arithmetic_v<T>)
					out_OnTrigger(static_cast<float>(out_Element));
				else
					out_OnTrigger(1.0f);
			}
			else
			{
				// Index out of bounds - output default and warn
				out_Element = T{};
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
			AddInEvent(IDs::Next, [this](float v) { m_NextFlag.SetDirty(); });
			AddInEvent(IDs::Reset, [this](float v) { m_ResetFlag.SetDirty(); });
			
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();

			// Initialize random generator with safe null checking
			// If in_Seed is null or -1, use high-resolution clock for random seed
			// Otherwise use the provided seed value
			int32_t seed;
			if (in_Seed && *in_Seed != -1) {
				seed = *in_Seed;
			} else {
				seed = static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
			}
			m_Random.SetSeed(seed);
			
			out_Value = T{};
		}

		void Process() final
		{
			if (m_NextFlag.CheckAndResetIfDirty())
				ProcessNext();

			if (m_ResetFlag.CheckAndResetIfDirty())
				ProcessReset();
		}

		//==========================================================================
		/// NodeProcessor setup  
		T* in_Min = nullptr;
		T* in_Max = nullptr;
		int32_t* in_Seed = nullptr;

		OutputEvent out_OnNext{ *this };
		OutputEvent out_OnReset{ *this };
		T out_Value{ T{} };

	private:
		Flag m_NextFlag;
		Flag m_ResetFlag;
		FastRandom m_Random;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessNext()
		{
			// Validate input pointers before dereferencing
			if (!in_Min || !in_Max)
			{
				// Handle null pointers gracefully - set default value and return
				out_Value = T{};
				OLO_CORE_WARN("Random: in_Min or in_Max is null, using default value");
				out_OnNext(1.0f);
				return;
			}

			T randomValue;
			
			// Get type-appropriate bounds and normalize min/max ordering
			T minValue, maxValue;
			
			if constexpr (std::is_same_v<T, float>)
			{
				// For float, clamp to reasonable audio range
				minValue = glm::clamp(*in_Min, -1000.0f, 1000.0f);
				maxValue = glm::clamp(*in_Max, -1000.0f, 1000.0f);
				
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
				minValue = glm::clamp(*in_Min, typeMin, typeMax);
				maxValue = glm::clamp(*in_Max, typeMin, typeMax);
				
				// Ensure min <= max
				if (minValue > maxValue)
					std::swap(minValue, maxValue);
					
				randomValue = static_cast<T>(m_Random.GetInt32InRange(static_cast<int32_t>(minValue), static_cast<int32_t>(maxValue)));
			}
			else
			{
				randomValue = T{};
			}
			
			out_Value = randomValue;
			out_OnNext(1.0f);
		}

		void ProcessReset()
		{
			// Safe seed handling with null checking
			// If in_Seed is null (initialization failed), use high-resolution clock as fallback
			// If in_Seed is -1, use high-resolution clock for truly random seed
			// Otherwise use the provided seed value
			int32_t seed;
			if (!in_Seed) {
				// Fallback: use high-resolution clock when in_Seed is null
				seed = static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
			} else {
				seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			}
			m_Random.SetSeed(seed);
			out_OnReset(1.0f);
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID