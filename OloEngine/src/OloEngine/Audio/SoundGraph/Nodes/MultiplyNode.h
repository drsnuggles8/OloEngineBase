#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Multiply node that multiplies InputA and InputB
	template<typename T>
	class MultiplyNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier InputA_ID = OLO_IDENTIFIER("InputA");
		const Identifier InputB_ID = OLO_IDENTIFIER("InputB");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		MultiplyNode()
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
			
			// Perform multiplication
			T result = inputA * inputB;
			
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
				return OLO_IDENTIFIER("MultiplyNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MultiplyNode_i32");
			else
				return OLO_IDENTIFIER("MultiplyNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Multiply (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Multiply (i32)";
			else
				return "Multiply (unknown)";
		}

	private:
		// Parameter IDs are available as members
		// InputA_ID, InputB_ID, Output_ID are accessible
	};

	// Common type aliases
	using MultiplyNodeF32 = MultiplyNode<f32>;
	using MultiplyNodeI32 = MultiplyNode<i32>;

} // namespace OloEngine::Audio::SoundGraph