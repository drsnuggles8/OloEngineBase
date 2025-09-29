#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <numbers>
#include <cmath>
#include <concepts>
#include <algorithm>

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
			UpdateSeconds();
		}

		void Process() final
		{
			UpdateSeconds();
		}

		//==========================================================================
		/// NodeProcessor setup
		float* in_BPM = nullptr;
		float out_Seconds = 1.0f;

	private:
		void RegisterEndpoints();
		void InitializeInputs();

		void UpdateSeconds()
		{
			// Safety check: ensure input pointer is valid and value is positive
			if (!in_BPM || *in_BPM <= 0.0f)
			{
				out_Seconds = 0.0f;
			}
			else
			{
				// Convert BPM to seconds per beat: 60 seconds per minute / beats per minute
				out_Seconds = 60.0f / (*in_BPM);
			}
		}
	};

	//==========================================================================
	/// NoteToFrequency - Convert MIDI note number to frequency in Hz
	//==========================================================================
	template<typename T>
		requires std::convertible_to<T, float>
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
			// Safety check: ensure input pointer is valid
			if (!in_MIDINote)
			{
				// Return default frequency (A4 = 440 Hz) when input is invalid
				out_Frequency = 440.0f;
				return;
			}

			// Clamp MIDI note to valid range (0-127) before conversion
			float note = std::clamp(static_cast<float>(*in_MIDINote), 0.0f, 127.0f);
			
			// Convert MIDI note to frequency using A4 (440 Hz) as reference (MIDI note 69)
			// Formula: frequency = 440 * 2^((note - 69) / 12)
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
			// Safety check: ensure input pointer is valid
			if (!in_Frequency)
			{
				// Return default MIDI note (A4 = 69) when input is invalid
				out_MIDINote = 69.0f;
				return;
			}

			// Validate frequency is above a safe minimum to prevent precision/domain issues with log2
			// Use audible floor of 20.0f Hz (below human hearing threshold) as minimum
			constexpr float minFrequency = 20.0f;
			
			if (*in_Frequency < minFrequency)
			{
				// Set to fallback for invalid or out-of-range inputs
				out_MIDINote = 0.0f; // MIDI note 0 (C-1, ~8.18 Hz)
				return;
			}

			// Convert frequency to MIDI note using A4 (440 Hz) as reference (MIDI note 69)
			// Formula: note = 69 + 12 * log2(frequency / 440)
			out_MIDINote = 69.0f + 12.0f * std::log2((*in_Frequency) / 440.0f);
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID