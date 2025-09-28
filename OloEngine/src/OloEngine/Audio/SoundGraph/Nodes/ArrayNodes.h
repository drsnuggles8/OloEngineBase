#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/FastRandom.h"

#include <vector>
#include <chrono>

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

			// Initialize random generator
			int32_t seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			m_Random.SetSeed(seed);
			
			out_Element = T{0};
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
		T out_Element{ T{0} };

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
				out_Element = T{0};
				OLO_CORE_WARN("GetRandom: Array is null or empty, using default value");
				return;
			}

			// Compute valid index range within array bounds
			int32_t arraySize = static_cast<int32_t>(in_Array->size());
			int32_t minIndex = (in_Min && *in_Min >= 0) ? glm::min(*in_Min, arraySize - 1) : 0;
			int32_t maxIndex = (in_Max && *in_Max < arraySize) ? *in_Max : arraySize - 1;
			
			// Ensure valid range
			if (minIndex > maxIndex)
			{
				minIndex = 0;
				maxIndex = arraySize - 1;
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
				out_Element = T{0};
				OLO_CORE_ERROR("GetRandom: Generated index {} out of bounds [0, {})", randomIndex, arraySize);
			}
		}

		void ProcessReset()
		{
			int32_t seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
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
			out_Element = T{0};
		}

		void Process() final
		{
			if (m_TriggerFlag.CheckAndResetIfDirty())
				ProcessTrigger();
		}

		//==========================================================================
		/// NodeProcessor setup
		T* in_Array = nullptr;
		int32_t* in_Index = nullptr;

		OutputEvent out_OnTrigger{ *this };
		T out_Element{ T{0} };

	private:
		Flag m_TriggerFlag;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessTrigger()
		{
			// Basic array access implementation
			// Note: Full choc::value array integration would provide dynamic arrays
			// For now, we implement single-element access with validation
			
			if (in_Array && in_Index)
			{
				// For single-element arrays, only index 0 is valid
				int32_t index = *in_Index;
				if (index == 0)
				{
					out_Element = *in_Array;
					out_OnTrigger(static_cast<float>(out_Element));
				}
				else
				{
					// Index out of bounds - output zero and don't trigger
					out_Element = T{0};
					OLO_CORE_WARN("ArrayGet: Index {} out of bounds for single-element array", index);
				}
			}
			else
			{
				// No input - output zero
				out_Element = T{0};
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

			// Initialize random generator
			int32_t seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			m_Random.SetSeed(seed);
			
			out_Value = T{0};
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
		T out_Value{ T{0} };

	private:
		Flag m_NextFlag;
		Flag m_ResetFlag;
		FastRandom m_Random;

		void RegisterEndpoints();
		void InitializeInputs();

		void ProcessNext()
		{
			T randomValue;
			
			if constexpr (std::is_same_v<T, float>)
			{
				randomValue = m_Random.GetFloatInRange(*in_Min, *in_Max);
			}
			else if constexpr (std::is_integral_v<T>)
			{
				randomValue = static_cast<T>(m_Random.GetInt32InRange(static_cast<int32_t>(*in_Min), static_cast<int32_t>(*in_Max)));
			}
			else
			{
				randomValue = T{0};
			}
			
			out_Value = randomValue;
			out_OnNext(1.0f);
		}

		void ProcessReset()
		{
			int32_t seed = (*in_Seed == -1) ? static_cast<int32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) : *in_Seed;
			m_Random.SetSeed(seed);
			out_OnReset(1.0f);
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID