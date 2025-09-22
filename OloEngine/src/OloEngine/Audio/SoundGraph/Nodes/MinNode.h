#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Min node that returns the minimum of InputA and InputB
	template<typename T>
	class MinNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier InputA_ID = OLO_IDENTIFIER("InputA");
		const Identifier InputB_ID = OLO_IDENTIFIER("InputB");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		MinNode()
		{
			// Register parameters directly
			AddParameter<T>(InputA_ID, "InputA", T{});
			AddParameter<T>(InputB_ID, "InputB", T{});
			AddParameter<T>(Output_ID, "Output", T{});
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Get current parameter values
			T inputA = GetParameterValue<T>(InputA_ID);
			T inputB = GetParameterValue<T>(InputB_ID);
			
			// Perform minimum operation
			T result = std::min(inputA, inputB);
			
			// Set output parameter
			SetParameterValue(Output_ID, result);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("MinNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MinNode_i32");
			else
				return OLO_IDENTIFIER("MinNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Min (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Min (i32)";
			else
				return "Min (unknown)";
		}

	private:
		// Parameter IDs are available as members
		// InputA_ID, InputB_ID, Output_ID are accessible
	};

	// Common type aliases
	using MinNodeF32 = MinNode<f32>;
	using MinNodeI32 = MinNode<i32>;

} // namespace OloEngine::Audio::SoundGraph