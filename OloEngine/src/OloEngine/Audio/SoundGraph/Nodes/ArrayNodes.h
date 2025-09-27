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
		T* in_Array = nullptr;
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
			// Generate random value in range
			T randomValue = static_cast<T>(m_Random.GetInt32InRange(*in_Min, *in_Max));
			out_Element = randomValue;
			out_OnNext(1.0f);
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
			// For now, since we don't have dynamic array support,
			// we'll treat this as a simple passthrough with bounds checking
			// TODO: Implement proper array handling when choc::value arrays are integrated
			
			if (in_Array)
			{
				out_Element = *in_Array;
			}
			
			out_OnTrigger(static_cast<float>(out_Element));
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