#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>
#include <type_traits>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// NoteToFrequencyNode - converts MIDI note numbers to frequencies
	/// Essential for musical note synthesis and pitch conversion
	/// Formula: frequency = 440.0 * 2^((note - 69) / 12)
	/// Where 69 = A4 (440Hz), the standard tuning reference
	template<typename T>
	class NoteToFrequencyNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "NoteToFrequencyNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_MIDINoteView;
		ValueView<f32> m_FrequencyView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentMIDINote = T{69}; // A4 default

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit NoteToFrequencyNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_MIDINoteView("MIDINote", T{69}) // A4
			, m_FrequencyView("Frequency", 440.0f) // A4 frequency
		{
			// Create Input/Output events
			RegisterInputEvent<T>("MIDINote", [this](const T& value) { m_CurrentMIDINote = value; });
			
			RegisterOutputEvent<f32>("Frequency");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_MIDINoteView.Initialize(maxBufferSize);
			m_FrequencyView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_MIDINoteView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current value from stream
				T midiNote = m_MIDINoteView.GetValue(sample);
				
				// Update internal state if changed
				if (midiNote != m_CurrentMIDINote) m_CurrentMIDINote = midiNote;
				
				// Convert MIDI note to frequency using equal temperament tuning
				// Formula: freq = 440 * 2^((note - 69) / 12) where 69 is A4 (440Hz)
				f32 noteOffset;
				if constexpr (std::is_same_v<T, f32>)
				{
					noteOffset = midiNote - 69.0f;
				}
				else if constexpr (std::is_same_v<T, i32>)
				{
					noteOffset = static_cast<f32>(midiNote) - 69.0f;
				}
				
				f32 frequency = 440.0f * std::pow(2.0f, noteOffset / 12.0f);
				
				// Clamp to reasonable audio range (avoid extreme frequencies)
				frequency = std::clamp(frequency, 0.1f, 22000.0f);
				
				// Set output value
				m_FrequencyView.SetValue(sample, frequency);
			}
			
			// Update output streams
			m_FrequencyView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetMIDINote(const T& value) { TriggerInputEvent<T>("MIDINote", value); }
		f32 GetFrequency() const 
		{ 
			f32 noteOffset;
			if constexpr (std::is_same_v<T, f32>)
			{
				noteOffset = m_CurrentMIDINote - 69.0f;
			}
			else
			{
				noteOffset = static_cast<f32>(m_CurrentMIDINote) - 69.0f;
			}
			f32 frequency = 440.0f * std::pow(2.0f, noteOffset / 12.0f);
			return std::clamp(frequency, 0.1f, 22000.0f);
		}
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<T>& GetMIDINoteView() { return m_MIDINoteView; }
		ValueView<f32>& GetFrequencyView() { return m_FrequencyView; }

		const ValueView<T>& GetMIDINoteView() const { return m_MIDINoteView; }
		const ValueView<f32>& GetFrequencyView() const { return m_FrequencyView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "MIDINote" << YAML::Value << m_CurrentMIDINote;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["MIDINote"]) m_CurrentMIDINote = node["MIDINote"].as<T>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "NoteToFrequencyNode<f32>";
			else if constexpr (std::is_same_v<T, i32>)
				return "NoteToFrequencyNode<i32>";
			else
				return "NoteToFrequencyNode<unknown>";
		}
	};

	// Common instantiations
	using NoteToFrequencyNodeF = NoteToFrequencyNode<f32>;
	using NoteToFrequencyNodeI = NoteToFrequencyNode<i32>;

} // namespace OloEngine::Audio::SoundGraph