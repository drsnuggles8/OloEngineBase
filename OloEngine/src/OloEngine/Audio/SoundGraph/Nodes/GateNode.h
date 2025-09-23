#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// GateNode - Gates signal based on control input
	/// Allows signal to pass through only when gate is open
	class GateNode : public NodeProcessor
	{
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Gate_ID = OLO_IDENTIFIER("Gate");
		const Identifier Threshold_ID = OLO_IDENTIFIER("Threshold");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		GateNode()
		{
			// Register parameters
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Gate_ID, "Gate", 0.0f);
			AddParameter<f32>(Threshold_ID, "Threshold", 0.5f); // Threshold for gate open
			AddParameter<f32>(Output_ID, "Output", 0.0f);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// No special initialization needed
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			f32 threshold = GetParameterValue<f32>(Threshold_ID, 0.5f);

			if (inputs && inputs[0] && inputs[1] && outputs && outputs[0])
			{
				// Process buffers: inputs[0] = signal, inputs[1] = gate
				for (u32 i = 0; i < numSamples; ++i)
				{
					f32 inputSample = inputs[0][i];
					f32 gateSample = inputs[1][i];
					
					// Gate is open if gate signal is above threshold
					bool gateOpen = gateSample > threshold;
					outputs[0][i] = gateOpen ? inputSample : 0.0f;
				}

				// Update output parameter with last sample
				SetParameterValue(Output_ID, outputs[0][numSamples - 1]);
			}
			else
			{
				// Process single values from parameters
				f32 inputValue = GetParameterValue<f32>(Input_ID, 0.0f);
				f32 gateValue = GetParameterValue<f32>(Gate_ID, 0.0f);
				
				bool gateOpen = gateValue > threshold;
				f32 result = gateOpen ? inputValue : 0.0f;
				
				SetParameterValue(Output_ID, result);

				// Fill output buffer if provided
				if (outputs && outputs[0])
				{
					for (u32 i = 0; i < numSamples; ++i)
					{
						outputs[0][i] = result;
					}
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("GateNode");
		}

		const char* GetDisplayName() const override
		{
			return "Gate";
		}

		// Utility methods
		f32 GetThreshold() const
		{
			return GetParameterValue<f32>(Threshold_ID, 0.5f);
		}

		bool IsGateOpen() const
		{
			f32 gateValue = GetParameterValue<f32>(Gate_ID, 0.0f);
			f32 threshold = GetParameterValue<f32>(Threshold_ID, 0.5f);
			return gateValue > threshold;
		}
	};
}