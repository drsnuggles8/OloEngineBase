#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Subtract node using reflection-based endpoint system
	template<typename T>
	class SubtractNode : public NodeProcessor
	{
	public:
		// Input members (pointers will be connected to parameter system)
		T* in_InputA = nullptr;
		T* in_InputB = nullptr;
		
		// Output member (direct value, computed in Process)
		T out_Output{};

		SubtractNode()
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
				out_Output = *in_InputA - *in_InputB;
			}
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
	};

	// Common type aliases
	using SubtractNodeF32 = SubtractNode<f32>;
	using SubtractNodeI32 = SubtractNode<i32>;

} // namespace OloEngine::Audio::SoundGraph

//==============================================================================
/// REFLECTION DESCRIPTIONS

// Describe the f32 version
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::SubtractNode<float>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::SubtractNode<float>::in_InputA,
		&OloEngine::Audio::SoundGraph::SubtractNode<float>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::SubtractNode<float>::out_Output)
);

// Describe the i32 version  
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::SubtractNode<int>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::SubtractNode<int>::in_InputA,
		&OloEngine::Audio::SoundGraph::SubtractNode<int>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::SubtractNode<int>::out_Output)
);