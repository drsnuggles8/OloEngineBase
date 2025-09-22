#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Modulo Node - calculates the remainder of value divided by modulo (value % modulo)
	/// Primarily designed for integer operations, but supports f32 using fmod
	template<typename T>
	class ModuloNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Value_ID = OLO_IDENTIFIER("Value");
		const Identifier Modulo_ID = OLO_IDENTIFIER("Modulo");
		const Identifier Result_ID = OLO_IDENTIFIER("Result");

	public:
		ModuloNode()
		{
			// Register parameters directly
			AddParameter<T>(Value_ID, "Value", T{0});
			AddParameter<T>(Modulo_ID, "Modulo", T{2}); // Default to modulo 2
			AddParameter<T>(Result_ID, "Result", T{0});
		}

		virtual ~ModuloNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			const T value = GetParameterValue<T>(Value_ID);
			const T modulo = GetParameterValue<T>(Modulo_ID);
			
			T result;
			if constexpr (std::is_same_v<T, f32>)
			{
				// Prevent division by zero
				if (modulo == 0.0f)
				{
					result = 0.0f; // Safe fallback value
				}
				else
				{
					result = std::fmod(value, modulo);
				}
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				// Prevent division by zero
				if (modulo == 0)
				{
					result = 0; // Safe fallback value
				}
				else
				{
					result = value % modulo;
				}
			}

			// Set output parameter
			SetParameterValue(Result_ID, result);

			// Fill output buffer if provided
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = static_cast<f32>(result);
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("ModuloNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("ModuloNodeI32");
			else
				return OLO_IDENTIFIER("ModuloNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Modulo (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Modulo (i32)";
			else
				return "Modulo";
		}
	};

	// Type aliases for common usage
	using ModuloNodeF32 = ModuloNode<f32>;
	using ModuloNodeI32 = ModuloNode<i32>;

} // namespace OloEngine::Audio::SoundGraph