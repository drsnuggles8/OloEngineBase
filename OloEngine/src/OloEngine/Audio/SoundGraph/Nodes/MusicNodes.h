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

// BPM constants for music timing calculations
namespace
{
	static constexpr float DEFAULT_BPM = 120.0f;
	static constexpr float MIN_BPM = 1.0f;
	static constexpr float MAX_BPM = 1000.0f;
}

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
			// Preserve existing output when BPM input is null
			if (!in_BPM)
				return;
			
			// Get BPM value and handle invalid/out-of-range values
			float bpm = *in_BPM;
			
			// Replace non-positive or out-of-range BPM with default
			if (bpm <= 0.0f || !std::isfinite(bpm))
			{
				bpm = DEFAULT_BPM;
			}
			else
			{
				// Clamp BPM to sane range
				bpm = std::clamp(bpm, MIN_BPM, MAX_BPM);
			}
			
			// Convert BPM to seconds per beat: 60 seconds per minute / beats per minute
			out_Seconds = 60.0f / bpm;
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

			// Handle non-positive frequencies
			if (*in_Frequency <= 0.0f)
			{
				// Set to fallback for invalid frequencies
				out_MIDINote = 0.0f; // MIDI note 0 (C-1)
				return;
			}

			// Convert frequency to MIDI note using A4 (440 Hz) as reference (MIDI note 69)
			// Formula: note = 69 + 12 * log2(frequency / 440)
			float midiNote = 69.0f + 12.0f * std::log2((*in_Frequency) / 440.0f);
			
			// Clamp to valid MIDI range [0.0f, 127.0f]
			out_MIDINote = std::clamp(midiNote, 0.0f, 127.0f);
		}
	};

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID