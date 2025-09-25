#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// BPMToSecondsNode - converts Beats Per Minute (BPM) to time duration in seconds
	/// Essential for music timing calculations and synchronization
	/// Formula: seconds = 60.0 / BPM
	class BPMToSecondsNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_BPMView;
		ValueView<f32> m_SecondsView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		f32 m_CurrentBPM = 120.0f; // Default to 120 BPM

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit BPMToSecondsNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_BPMView("BPM", 120.0f)
			, m_SecondsView("Seconds", 0.5f) // Default result for 120 BPM
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("BPM", [this](f32 value) { m_CurrentBPM = value; });
			
			RegisterOutputEvent<f32>("Seconds");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_BPMView.Initialize(maxBufferSize);
			m_SecondsView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_BPMView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current value from stream
				f32 bpm = m_BPMView.GetValue(sample);
				
				// Update internal state if changed
				if (bpm != m_CurrentBPM) m_CurrentBPM = bpm;
				
				// Convert BPM to seconds per beat with division-by-zero protection
				f32 seconds;
				if (bpm <= 0.0f)
				{
					seconds = 60.0f / 120.0f; // Default to 120 BPM when invalid
				}
				else
				{
					seconds = 60.0f / bpm;
				}
				
				// Set output value
				m_SecondsView.SetValue(sample, seconds);
			}
			
			// Update output streams
			m_SecondsView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Methods (for compatibility with existing code)
		//======================================================================
		
		void SetBPM(f32 value) { TriggerInputEvent<f32>("BPM", value); }
		f32 GetSeconds() const 
		{ 
			if (m_CurrentBPM <= 0.0f) return 60.0f / 120.0f;
			return 60.0f / m_CurrentBPM; 
		}
		
		//======================================================================
		// ValueView Stream Access (for audio connections)
		//======================================================================
		
		ValueView<f32>& GetBPMView() { return m_BPMView; }
		ValueView<f32>& GetSecondsView() { return m_SecondsView; }

		const ValueView<f32>& GetBPMView() const { return m_BPMView; }
		const ValueView<f32>& GetSecondsView() const { return m_SecondsView; }

		//======================================================================
		// Serialization
		//======================================================================
		
		void Serialize(YAML::Emitter& out) const override
		{
			NodeProcessor::Serialize(out);
			out << YAML::Key << "BPM" << YAML::Value << m_CurrentBPM;
		}

		void Deserialize(const YAML::Node& node) override
		{
			NodeProcessor::Deserialize(node);
			if (node["BPM"]) m_CurrentBPM = node["BPM"].as<f32>();
		}

		//======================================================================
		// Node Information
		//======================================================================
		
		std::string GetTypeName() const override
		{
			return "BPMToSecondsNode";
		}
	};

} // namespace OloEngine::Audio::SoundGraph