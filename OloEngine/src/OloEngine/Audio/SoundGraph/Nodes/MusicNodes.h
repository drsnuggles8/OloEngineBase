#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <numbers>
#include <cmath>

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
	//==========================================================================
	/// BPMToSeconds - Convert beats per minute to seconds per beat
	//==========================================================================
	struct BPMToSeconds : public NodeProcessor
	{
		explicit BPMToSeconds(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();
			out_Seconds = (*in_BPM) <= 0.0f ? 0.0f : 60.0f / (*in_BPM);
		}

		void Process() final
		{
			out_Seconds = (*in_BPM) <= 0.0f ? 0.0f : 60.0f / (*in_BPM);
		}

		//==========================================================================
		/// NodeProcessor setup
		float* in_BPM = nullptr;
		float out_Seconds = 1.0f;

	private:
		void RegisterEndpoints();
		void InitializeInputs();
	};

	//==========================================================================
	/// NoteToFrequency - Convert MIDI note number to frequency in Hz
	//==========================================================================
	template<typename T>
	struct NoteToFrequency : public NodeProcessor
	{
		explicit NoteToFrequency(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();
			CalculateFrequency();
		}

		void Process() final
		{
			CalculateFrequency();
		}

		//==========================================================================
		/// NodeProcessor setup
		T* in_MIDINote = nullptr;
		float out_Frequency = 440.0f;

	private:
		void RegisterEndpoints();
		void InitializeInputs();

		void CalculateFrequency()
		{
			// Convert MIDI note to frequency using A4 (440 Hz) as reference (MIDI note 69)
			// Formula: frequency = 440 * 2^((note - 69) / 12)
			float note = static_cast<float>(*in_MIDINote);
			out_Frequency = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
		}
	};

	//==========================================================================
	/// FrequencyToNote - Convert frequency in Hz to MIDI note number
	//==========================================================================
	struct FrequencyToNote : public NodeProcessor
	{
		explicit FrequencyToNote(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			RegisterEndpoints();
		}

		void Init() final
		{
			InitializeInputs();
			CalculateNote();
		}

		void Process() final
		{
			CalculateNote();
		}

		//==========================================================================
		/// NodeProcessor setup
		float* in_Frequency = nullptr;
		float out_MIDINote = 69.0f;

	private:
		void RegisterEndpoints();
		void InitializeInputs();

		void CalculateNote()
		{
			// Convert frequency to MIDI note using A4 (440 Hz) as reference (MIDI note 69)
			// Formula: note = 69 + 12 * log2(frequency / 440)
			if (*in_Frequency > 0.0f)
			{
				out_MIDINote = 69.0f + 12.0f * std::log2((*in_Frequency) / 440.0f);
			}
			else
			{
				out_MIDINote = 0.0f;
			}
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID