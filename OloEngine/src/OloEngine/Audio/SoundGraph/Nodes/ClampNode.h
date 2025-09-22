#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Clamp node that constrains Value between Min and Max
	template<typename T>
	class ClampNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Value_ID = OLO_IDENTIFIER("Value");
		const Identifier Min_ID = OLO_IDENTIFIER("Min");
		const Identifier Max_ID = OLO_IDENTIFIER("Max");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		ClampNode()
		{
			// Register parameters directly
			AddParameter<T>(Value_ID, "Value", T{});
			AddParameter<T>(Min_ID, "Min", T{});
			AddParameter<T>(Max_ID, "Max", T{});
			AddParameter<T>(Output_ID, "Output", T{});
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Get current parameter values
			T value = GetParameterValue<T>(Value_ID);
			T minValue = GetParameterValue<T>(Min_ID);
			T maxValue = GetParameterValue<T>(Max_ID);
			
			// Perform clamp operation - ensure min <= max for proper clamping
			T result = std::clamp(value, std::min(minValue, maxValue), std::max(minValue, maxValue));
			
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
				return OLO_IDENTIFIER("ClampNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("ClampNode_i32");
			else
				return OLO_IDENTIFIER("ClampNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Clamp (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Clamp (i32)";
			else
				return "Clamp (unknown)";
		}

	private:
		// Parameter IDs are available as members
		// Value_ID, Min_ID, Max_ID, Output_ID are accessible
	};

	// Common type aliases
	using ClampNodeF32 = ClampNode<f32>;
	using ClampNodeI32 = ClampNode<i32>;

} // namespace OloEngine::Audio::SoundGraph