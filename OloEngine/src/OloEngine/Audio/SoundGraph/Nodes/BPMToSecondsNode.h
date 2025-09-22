#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// BPMToSeconds Node - converts Beats Per Minute (BPM) to time duration in seconds
	/// Essential for music timing calculations and synchronization
	/// Formula: seconds = 60.0 / BPM
	class BPMToSecondsNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier BPM_ID = OLO_IDENTIFIER("BPM");
		const Identifier Seconds_ID = OLO_IDENTIFIER("Seconds");

	public:
		BPMToSecondsNode()
		{
			// Register parameters directly
			AddParameter<f32>(BPM_ID, "BPM", 120.0f);      // Default to 120 BPM (common tempo)
			AddParameter<f32>(Seconds_ID, "Seconds", 0.5f); // Default result for 120 BPM
		}

		virtual ~BPMToSecondsNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const f32 bpm = GetParameterValue<f32>(BPM_ID);
			
			f32 seconds;
			// Prevent division by zero and negative BPM
			if (bpm <= 0.0f)
			{
				seconds = 60.0f / 120.0f; // Default to 120 BPM when invalid input provided
			}
			else
			{
				// Convert BPM to seconds per beat
				seconds = 60.0f / bpm;
			}

			// Set output parameter
			SetParameterValue(Seconds_ID, seconds);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = seconds;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("BPMToSecondsNode");
		}

		const char* GetDisplayName() const override
		{
			return "BPM to Seconds";
		}
	};

} // namespace OloEngine::Audio::SoundGraph