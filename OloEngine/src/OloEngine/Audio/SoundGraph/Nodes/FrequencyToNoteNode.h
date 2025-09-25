#pragma once

#include "../NodeProcessor.h"
#include "../ValueView.h"
#include "../InputView.h"
#include "../OutputView.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// FrequencyToNote Node - converts frequencies back to MIDI note numbers
	/// Essential for pitch detection and music analysis applications
	/// Formula: note = 69 + 12 * log2(frequency / 440.0)
	/// Where 69 = A4 (440Hz), the standard tuning reference
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class FrequencyToNoteNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_FrequencyView;
		
		// Output streams
		OutputView<f32> m_MIDINoteView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentFrequency = 440.0f;  // A4 frequency
		f32 m_CurrentMIDINote = 69.0f;    // A4 MIDI note

		// Constants for frequency to note conversion
		static constexpr f32 REFERENCE_FREQUENCY = 440.0f;  // A4
		static constexpr f32 REFERENCE_NOTE = 69.0f;        // A4 MIDI note
		static constexpr f32 MIN_MIDI_NOTE = 0.0f;
		static constexpr f32 MAX_MIDI_NOTE = 127.0f;

	public:
		FrequencyToNoteNode()
			: m_FrequencyView([this](f32 value) { m_CurrentFrequency = value; }),
			  m_MIDINoteView([this](f32 value) { m_CurrentMIDINote = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		virtual ~FrequencyToNoteNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_FrequencyView.Initialize(maxBufferSize);
			m_MIDINoteView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current frequency value from stream
				f32 frequency = m_FrequencyView.GetValue(sample);
				
				// Update internal state
				m_CurrentFrequency = frequency;
				
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
					const f32 frequencyRatio = frequency / REFERENCE_FREQUENCY;
					midiNote = REFERENCE_NOTE + 12.0f * glm::log2(frequencyRatio);
					
					// Clamp to valid MIDI note range (0-127)
					midiNote = glm::clamp(midiNote, MIN_MIDI_NOTE, MAX_MIDI_NOTE);
				}
				
				// Update current state
				m_CurrentMIDINote = midiNote;
				
				// Set output value
				m_MIDINoteView.SetValue(sample, midiNote);
			}
			
			// Update output streams
			m_MIDINoteView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("FrequencyToNoteNode");
		}

		const char* GetDisplayName() const override
		{
			return "Frequency to Note";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MIDINote")) m_CurrentMIDINote = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			else if (id == OLO_IDENTIFIER("MIDINote")) return static_cast<T>(m_CurrentMIDINote);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Convert a single frequency value to MIDI note
		static f32 FrequencyToMIDINote(f32 frequency)
		{
			if (frequency <= 0.0f) return 0.0f;
			
			const f32 frequencyRatio = frequency / REFERENCE_FREQUENCY;
			f32 midiNote = REFERENCE_NOTE + 12.0f * glm::log2(frequencyRatio);
			return glm::clamp(midiNote, MIN_MIDI_NOTE, MAX_MIDI_NOTE);
		}

		/// Get the current frequency value
		f32 GetCurrentFrequency() const
		{
			return m_CurrentFrequency;
		}

		/// Get the current MIDI note value
		f32 GetCurrentMIDINote() const
		{
			return m_CurrentMIDINote;
		}
	};

} // namespace OloEngine::Audio::SoundGraph