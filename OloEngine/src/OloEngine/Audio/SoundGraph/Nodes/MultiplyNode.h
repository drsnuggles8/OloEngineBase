#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Multiply node using reflection-based endpoint system
	template<typename T>
	class MultiplyNode : public NodeProcessor
	{
	public:
		// Input members (pointers will be connected to parameter system)
		T* in_InputA = nullptr;
		T* in_InputB = nullptr;
		
		// Output member (direct value, computed in Process)
		T out_Output{};

		MultiplyNode()
		{
			// Automatic endpoint registration using reflection
			EndpointUtilities::RegisterEndpoints(this);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize input pointers to connect with parameter system
			EndpointUtilities::InitializeInputs(this);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Use the connected input values (now accessible via pointers)
			if (in_InputA && in_InputB)
			{
				out_Output = *in_InputA * *in_InputB;
			}
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
	};

	// Common type aliases
	using MultiplyNodeF32 = MultiplyNode<f32>;
	using MultiplyNodeI32 = MultiplyNode<i32>;

} // namespace OloEngine::Audio::SoundGraph

//==============================================================================
/// REFLECTION DESCRIPTIONS

// Describe the f32 version
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::MultiplyNode<float>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::MultiplyNode<float>::in_InputA,
		&OloEngine::Audio::SoundGraph::MultiplyNode<float>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::MultiplyNode<float>::out_Output)
);

// Describe the i32 version  
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::MultiplyNode<int>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::MultiplyNode<int>::in_InputA,
		&OloEngine::Audio::SoundGraph::MultiplyNode<int>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::MultiplyNode<int>::out_Output)
);