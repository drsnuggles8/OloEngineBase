#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Divide node using reflection-based endpoint system with division-by-zero protection
	template<typename T>
	class DivideNode : public NodeProcessor
	{
	public:
		// Input members (pointers will be connected to parameter system)
		T* in_InputA = nullptr;
		T* in_InputB = nullptr;
		
		// Output member (direct value, computed in Process)
		T out_Output{};

		DivideNode()
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
				// Handle division by zero
				T result{};
				if constexpr (std::is_floating_point_v<T>)
				{
					// For floating point types, check for near-zero values
					if (std::abs(*in_InputB) < std::numeric_limits<T>::epsilon())
					{
						result = *in_InputA >= T{} ? std::numeric_limits<T>::infinity() : -std::numeric_limits<T>::infinity();
					}
					else
					{
						result = *in_InputA / *in_InputB;
					}
				}
				else
				{
					// For integer types, check for exact zero
					if (*in_InputB == T{})
					{
						result = T{}; // Return zero for integer division by zero
					}
					else
					{
						result = *in_InputA / *in_InputB;
					}
				}
				
				out_Output = result;
			}
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("DivideNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("DivideNode_i32");
			else
				return OLO_IDENTIFIER("DivideNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Divide (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Divide (i32)";
			else
				return "Divide (unknown)";
		}
	};

	// Common type aliases
	using DivideNodeF32 = DivideNode<f32>;
	using DivideNodeI32 = DivideNode<i32>;

} // namespace OloEngine::Audio::SoundGraph

//==============================================================================
/// REFLECTION DESCRIPTIONS

// Describe the f32 version
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::DivideNode<float>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::DivideNode<float>::in_InputA,
		&OloEngine::Audio::SoundGraph::DivideNode<float>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::DivideNode<float>::out_Output)
);

// Describe the i32 version  
DESCRIBE_NODE(OloEngine::Audio::SoundGraph::DivideNode<int>,
	NODE_INPUTS(
		&OloEngine::Audio::SoundGraph::DivideNode<int>::in_InputA,
		&OloEngine::Audio::SoundGraph::DivideNode<int>::in_InputB),
	NODE_OUTPUTS(
		&OloEngine::Audio::SoundGraph::DivideNode<int>::out_Output)
);