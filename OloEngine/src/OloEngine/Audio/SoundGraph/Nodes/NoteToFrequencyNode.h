#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// NoteToFrequency Node - converts MIDI note numbers to frequencies
	/// Essential for musical note synthesis and pitch conversion
	/// Formula: frequency = 440.0 * 2^((note - 69) / 12)
	/// Where 69 = A4 (440Hz), the standard tuning reference
	template<typename T>
	class NoteToFrequencyNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier MIDINote_ID = OLO_IDENTIFIER("MIDINote");
		const Identifier Frequency_ID = OLO_IDENTIFIER("Frequency");

	public:
		NoteToFrequencyNode()
		{
			// Register parameters directly
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(MIDINote_ID, "MIDINote", 69.0f);    // A4 (440Hz)
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(MIDINote_ID, "MIDINote", 69);       // A4 (440Hz)
			}
			AddParameter<f32>(Frequency_ID, "Frequency", 440.0f);     // A4 frequency
		}

		virtual ~NoteToFrequencyNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const T midiNote = GetParameterValue<T>(MIDINote_ID);
			
			f32 frequency;
			
			// Convert MIDI note to frequency using equal temperament tuning
			// Standard formula: freq = 440 * 2^((note - 69) / 12)
			// Where 69 is A4 (440Hz)
			if constexpr (std::is_same_v<T, f32>)
			{
				const f32 noteOffset = midiNote - 69.0f;
				frequency = 440.0f * glm::exp2(noteOffset / 12.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				const f32 noteOffset = static_cast<f32>(midiNote) - 69.0f;
				frequency = 440.0f * glm::exp2(noteOffset / 12.0f);
			}

			// Clamp to reasonable audio range (avoid extreme frequencies)
			frequency = glm::clamp(frequency, 0.1f, 22000.0f);

			// Set output parameter
			SetParameterValue(Frequency_ID, frequency);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = frequency;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("NoteToFrequencyNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("NoteToFrequencyNodeI32");
			else
				return OLO_IDENTIFIER("NoteToFrequencyNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Note to Frequency (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Note to Frequency (i32)";
			else
				return "Note to Frequency";
		}
	};

	// Type aliases for common usage
	using NoteToFrequencyNodeF32 = NoteToFrequencyNode<f32>;
	using NoteToFrequencyNodeI32 = NoteToFrequencyNode<i32>;

} // namespace OloEngine::Audio::SoundGraph