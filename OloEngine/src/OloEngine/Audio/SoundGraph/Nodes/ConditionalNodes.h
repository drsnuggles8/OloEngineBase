#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// IfElseNode - Conditional node that outputs one of two values based on condition
	template<typename T>
	class IfElseNode : public NodeProcessor
	{
	private:
		const Identifier Condition_ID = OLO_IDENTIFIER("Condition");
		const Identifier TrueValue_ID = OLO_IDENTIFIER("TrueValue");
		const Identifier FalseValue_ID = OLO_IDENTIFIER("FalseValue");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		IfElseNode()
		{
			AddParameter<f32>(Condition_ID, "Condition", 0.0f);
			AddParameter<T>(TrueValue_ID, "True Value", T{});
			AddParameter<T>(FalseValue_ID, "False Value", T{});
			AddParameter<T>(Output_ID, "Output", T{});
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			f32 condition = GetParameterValue<f32>(Condition_ID, 0.0f);
			T trueValue = GetParameterValue<T>(TrueValue_ID, T{});
			T falseValue = GetParameterValue<T>(FalseValue_ID, T{});
			
			T result = (condition > 0.5f) ? trueValue : falseValue;
			SetParameterValue(Output_ID, result);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("IfElseNode");
		}

		const char* GetDisplayName() const override
		{
			return "If Else";
		}
	};

	//==============================================================================
	/// BoolToFloatNode - Converts boolean values to float (0.0 or 1.0)
	class BoolToFloatNode : public NodeProcessor
	{
	private:
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		BoolToFloatNode()
		{
			AddParameter<f32>(Input_ID, "Input", 0.0f);
			AddParameter<f32>(Output_ID, "Output", 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			f32 input = GetParameterValue<f32>(Input_ID, 0.0f);
			f32 result = (input > 0.5f) ? 1.0f : 0.0f;
			SetParameterValue(Output_ID, result);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("BoolToFloatNode");
		}

		const char* GetDisplayName() const override
		{
			return "Bool To Float";
		}
	};

	//==============================================================================
	/// IntToFloatNode - Converts integer values to float
	class IntToFloatNode : public NodeProcessor
	{
	private:
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

	public:
		IntToFloatNode()
		{
			AddParameter<i32>(Input_ID, "Input", 0);
			AddParameter<f32>(Output_ID, "Output", 0.0f);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			i32 input = GetParameterValue<i32>(Input_ID, 0);
			f32 result = static_cast<f32>(input);
			SetParameterValue(Output_ID, result);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("IntToFloatNode");
		}

		const char* GetDisplayName() const override
		{
			return "Int To Float";
		}
	};

	// Type aliases
	using IfElseNodeF32 = IfElseNode<f32>;
	using IfElseNodeI32 = IfElseNode<i32>;
}