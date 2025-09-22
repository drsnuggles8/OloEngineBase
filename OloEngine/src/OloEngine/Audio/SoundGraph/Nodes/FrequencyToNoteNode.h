#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// FrequencyToNote Node - converts frequencies back to MIDI note numbers
	/// Essential for pitch detection and music analysis applications
	/// Formula: note = 69 + 12 * log2(frequency / 440.0)
	/// Where 69 = A4 (440Hz), the standard tuning reference
	class FrequencyToNoteNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");
		const Identifier MIDINote_ID = OLO_IDENTIFIER("MIDINote");

	public:
		FrequencyToNoteNode()
		{
			// Register parameters directly
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);  // A4 frequency
			AddParameter<f32>(MIDINote_ID, "MIDINote", 69.0f);     // A4 MIDI note
		}

		virtual ~FrequencyToNoteNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 frequency = GetParameterValue<f32>(Frequency_ID);
			
			f32 midiNote;
			
			// Prevent logarithm of zero or negative frequencies
			if (frequency <= 0.0f)
			{
				midiNote = 0.0f; // Safe fallback for invalid frequency
			}
			else
			{
				// Convert frequency to MIDI note using equal temperament tuning
				// Formula: note = 69 + 12 * log2(freq / 440)
				// Where 69 is A4 (440Hz)
				const f32 frequencyRatio = frequency / 440.0f;
				midiNote = 69.0f + 12.0f * glm::log2(frequencyRatio);
				
				// Clamp to valid MIDI note range (0-127)
				midiNote = glm::clamp(midiNote, 0.0f, 127.0f);
			}

			// Set output parameter
			SetParameterValue(MIDINote_ID, midiNote);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = midiNote;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("FrequencyToNoteNode");
		}

		const char* GetDisplayName() const override
		{
			return "Frequency to Note";
		}
	};

} // namespace OloEngine::Audio::SoundGraph