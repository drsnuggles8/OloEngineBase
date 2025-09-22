#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Divide node that divides InputA by InputB with division-by-zero protection
	template<typename T>
	class DivideNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier InputA_ID = OLO_IDENTIFIER("InputA");
		const Identifier InputB_ID = OLO_IDENTIFIER("InputB");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		DivideNode()
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
			
			// Handle division by zero
			T result{};
			if constexpr (std::is_floating_point_v<T>)
			{
				// For floating point types, check for near-zero values
				if (std::abs(inputB) < std::numeric_limits<T>::epsilon())
				{
					result = inputA >= T{} ? std::numeric_limits<T>::infinity() : -std::numeric_limits<T>::infinity();
				}
				else
				{
					result = inputA / inputB;
				}
			}
			else
			{
				// For integer types, check for exact zero
				if (inputB == T{})
				{
					result = T{}; // Return zero for integer division by zero
				}
				else
				{
					result = inputA / inputB;
				}
			}
			
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

	private:
		// Parameter IDs are available as members
		// InputA_ID, InputB_ID, Output_ID are accessible
	};

	// Common type aliases
	using DivideNodeF32 = DivideNode<f32>;
	using DivideNodeI32 = DivideNode<i32>;

} // namespace OloEngine::Audio::SoundGraph