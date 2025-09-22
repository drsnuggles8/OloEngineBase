#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Subtract node that subtracts InputB from InputA
	template<typename T>
	class SubtractNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier InputA_ID = OLO_IDENTIFIER("InputA");
		const Identifier InputB_ID = OLO_IDENTIFIER("InputB");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		SubtractNode()
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
			
			// Perform subtraction
			T result = inputA - inputB;
			
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
				return OLO_IDENTIFIER("SubtractNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("SubtractNode_i32");
			else
				return OLO_IDENTIFIER("SubtractNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Subtract (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Subtract (i32)";
			else
				return "Subtract (unknown)";
		}

	private:
		// Parameter IDs are available as members
		// InputA_ID, InputB_ID, Output_ID are accessible
	};

	// Common type aliases
	using SubtractNodeF32 = SubtractNode<f32>;
	using SubtractNodeI32 = SubtractNode<i32>;

} // namespace OloEngine::Audio::SoundGraph