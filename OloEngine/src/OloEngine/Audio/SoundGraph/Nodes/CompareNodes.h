#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// EqualNode - Template node for equality comparison
	/// Based on Hazel's comparison nodes
	template<typename T>
	class EqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, bool>, "EqualNode can only be used with arithmetic types or bool");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		EqualNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				AddParameter<bool>(LeftInput_ID, "Left Input", false);
				AddParameter<bool>(RightInput_ID, "Right Input", false);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				SetParameterValue(LeftInput_ID, false);
				SetParameterValue(RightInput_ID, false);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue == rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("EqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("EqualNodeI32");
			else if constexpr (std::is_same_v<T, bool>)
				return OLO_IDENTIFIER("EqualNodeBool");
			else
				return OLO_IDENTIFIER("EqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Equal"; }
	};

	//==============================================================================
	/// GreaterThanNode - Template node for greater than comparison
	template<typename T>
	class GreaterThanNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GreaterThanNode can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		GreaterThanNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue > rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("GreaterThanNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("GreaterThanNodeI32");
			else
				return OLO_IDENTIFIER("GreaterThanNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Greater Than"; }
	};

	//==============================================================================
	/// LessThanNode - Template node for less than comparison
	template<typename T>
	class LessThanNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "LessThanNode can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		LessThanNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue < rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LessThanNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LessThanNodeI32");
			else
				return OLO_IDENTIFIER("LessThanNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Less Than"; }
	};

	//==============================================================================
	/// GreaterThanOrEqualNode - Template node for greater than or equal comparison
	template<typename T>
	class GreaterThanOrEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GreaterThanOrEqualNode can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		GreaterThanOrEqualNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue >= rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeI32");
			else
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Greater Than Or Equal"; }
	};

	//==============================================================================
	/// LessThanOrEqualNode - Template node for less than or equal comparison
	template<typename T>
	class LessThanOrEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "LessThanOrEqualNode can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		LessThanOrEqualNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue <= rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LessThanOrEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LessThanOrEqualNodeI32");
			else
				return OLO_IDENTIFIER("LessThanOrEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Less Than Or Equal"; }
	};

	//==============================================================================
	/// NotEqualNode - Template node for inequality comparison
	template<typename T>
	class NotEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "NotEqualNode can only be used with arithmetic types");

	private:
		// Endpoint identifiers
		const Identifier LeftInput_ID = OLO_IDENTIFIER("LeftInput");
		const Identifier RightInput_ID = OLO_IDENTIFIER("RightInput");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		NotEqualNode()
		{
			// Register parameters
			if constexpr (std::is_same_v<T, f32>)
			{
				AddParameter<f32>(LeftInput_ID, "Left Input", 0.0f);
				AddParameter<f32>(RightInput_ID, "Right Input", 0.0f);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				AddParameter<i32>(LeftInput_ID, "Left Input", 0);
				AddParameter<i32>(RightInput_ID, "Right Input", 0);
				AddParameter<f32>(Output_ID, "Output", 0.0f);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			// Set default values
			if constexpr (std::is_same_v<T, f32>)
			{
				SetParameterValue(LeftInput_ID, 0.0f);
				SetParameterValue(RightInput_ID, 0.0f);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				SetParameterValue(LeftInput_ID, 0);
				SetParameterValue(RightInput_ID, 0);
			}
			SetParameterValue(Output_ID, 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			T leftValue = GetParameterValue<T>(LeftInput_ID);
			T rightValue = GetParameterValue<T>(RightInput_ID);
			
			f32 result = (leftValue != rightValue) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("NotEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("NotEqualNodeI32");
			else
				return OLO_IDENTIFIER("NotEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Not Equal"; }
	};

	// Type aliases for convenience
	using EqualNodeF32 = EqualNode<f32>;
	using EqualNodeI32 = EqualNode<i32>;
	using GreaterThanNodeF32 = GreaterThanNode<f32>;
	using GreaterThanNodeI32 = GreaterThanNode<i32>;
	using LessThanNodeF32 = LessThanNode<f32>;
	using LessThanNodeI32 = LessThanNode<i32>;
	using GreaterThanOrEqualNodeF32 = GreaterThanOrEqualNode<f32>;
	using GreaterThanOrEqualNodeI32 = GreaterThanOrEqualNode<i32>;
	using LessThanOrEqualNodeF32 = LessThanOrEqualNode<f32>;
	using LessThanOrEqualNodeI32 = LessThanOrEqualNode<i32>;
	using NotEqualNodeF32 = NotEqualNode<f32>;
	using NotEqualNodeI32 = NotEqualNode<i32>;

}