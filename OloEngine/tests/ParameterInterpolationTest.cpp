#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Parameters.h"
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Nodes/SineNode.h"

using namespace OloEngine::Audio::SoundGraph;

//==============================================================================
/// Test InterpolatedParameter class
TEST(ParameterInterpolationTest, InterpolatedParameterBasics)
{
	// Create an interpolated parameter
	auto param = InterpolatedParameter<f32>(OLO_IDENTIFIER("TestParam"), "TestParam", 0.0f);
	
	// Set interpolation config
	InterpolationConfig config;
	config.SampleRate = 48000.0;
	config.InterpolationSamples = 48; // 1ms at 48kHz
	config.EnableInterpolation = true;
	param.SetInterpolationConfig(config);
	
	// Initial value should be 0.0f
	EXPECT_FLOAT_EQ(param.Value, 0.0f);
	EXPECT_FLOAT_EQ(param.GetCurrentValue(), 0.0f);
	EXPECT_FLOAT_EQ(param.GetTargetValue(), 0.0f);
	EXPECT_FALSE(param.IsInterpolating());
	
	// Set target value with interpolation
	param.SetTargetValue(1.0f, true);
	EXPECT_FLOAT_EQ(param.GetTargetValue(), 1.0f);
	EXPECT_TRUE(param.IsInterpolating());
	EXPECT_FLOAT_EQ(param.GetInterpolationProgress(), 0.0f); // Should start at 0
	
	// Process one interpolation step to verify progress
	param.ProcessInterpolation();
	EXPECT_GT(param.GetInterpolationProgress(), 0.0f);
	EXPECT_LT(param.GetInterpolationProgress(), 1.0f);
	
	// Process interpolation steps
	for (int i = 0; i < 48; ++i)
	{
		param.ProcessInterpolation();
	}
	
	// Should reach target value
	EXPECT_FLOAT_EQ(param.Value, 1.0f);
	EXPECT_FLOAT_EQ(param.GetCurrentValue(), 1.0f);
	EXPECT_FALSE(param.IsInterpolating());
	EXPECT_FLOAT_EQ(param.GetInterpolationProgress(), 1.0f);
}

//==============================================================================
/// Test immediate parameter setting
TEST(ParameterInterpolationTest, ImmediateParameterSetting)
{
	auto param = InterpolatedParameter<f32>(OLO_IDENTIFIER("TestParam"), "TestParam", 0.0f);
	
	// Set interpolation config
	InterpolationConfig config;
	config.SampleRate = 48000.0;
	config.InterpolationSamples = 48;
	config.EnableInterpolation = true;
	param.SetInterpolationConfig(config);
	
	// Set target value without interpolation
	param.SetTargetValue(1.0f, false);
	EXPECT_FLOAT_EQ(param.Value, 1.0f);
	EXPECT_FLOAT_EQ(param.GetCurrentValue(), 1.0f);
	EXPECT_FLOAT_EQ(param.GetTargetValue(), 1.0f);
	EXPECT_FALSE(param.IsInterpolating());
}

//==============================================================================
/// Test ParameterRegistry with interpolated parameters
TEST(ParameterInterpolationTest, ParameterRegistryInterpolation)
{
	ParameterRegistry registry;
	
	// Add regular and interpolated parameters
	registry.AddParameter<f32>(OLO_IDENTIFIER("RegularParam"), "RegularParam", 0.0f);
	registry.AddInterpolatedParameter<f32>(OLO_IDENTIFIER("InterpParam"), "InterpParam", 0.0f);
	
	// Set interpolation config
	InterpolationConfig config;
	config.SampleRate = 48000.0;
	config.InterpolationSamples = 10;
	config.EnableInterpolation = true;
	registry.SetInterpolationConfig(config);
	
	// Check parameter types
	EXPECT_TRUE(registry.HasParameter(OLO_IDENTIFIER("RegularParam")));
	EXPECT_TRUE(registry.HasParameter(OLO_IDENTIFIER("InterpParam")));
	EXPECT_FALSE(registry.ParameterSupportsInterpolation(OLO_IDENTIFIER("RegularParam")));
	EXPECT_TRUE(registry.ParameterSupportsInterpolation(OLO_IDENTIFIER("InterpParam")));
	
	// Set values with interpolation
	registry.SetParameterValue<f32>(OLO_IDENTIFIER("RegularParam"), 1.0f, true);
	registry.SetParameterValue<f32>(OLO_IDENTIFIER("InterpParam"), 1.0f, true);
	
	// Regular parameter should be immediate
	EXPECT_FLOAT_EQ(registry.GetParameterValue<f32>(OLO_IDENTIFIER("RegularParam")), 1.0f);
	
	// Interpolated parameter should start interpolating
	EXPECT_LT(registry.GetParameterValue<f32>(OLO_IDENTIFIER("InterpParam")), 1.0f);
	
	// Process interpolation
	for (int i = 0; i < 10; ++i)
	{
		registry.ProcessInterpolation();
	}
	
	// Both should now be at target value
	EXPECT_FLOAT_EQ(registry.GetParameterValue<f32>(OLO_IDENTIFIER("RegularParam")), 1.0f);
	EXPECT_FLOAT_EQ(registry.GetParameterValue<f32>(OLO_IDENTIFIER("InterpParam")), 1.0f);
}

//==============================================================================
/// Test SineNode with interpolated frequency
TEST(ParameterInterpolationTest, SineNodeInterpolatedFrequency)
{
	SineNode sineNode;
	
	// Initialize the node
	f64 sampleRate = 48000.0;
	u32 bufferSize = 64;
	sineNode.Initialize(sampleRate, bufferSize);
	
	// Check that frequency parameter exists and supports interpolation
	EXPECT_TRUE(sineNode.HasParameter(OLO_IDENTIFIER("Frequency")));
	EXPECT_TRUE(sineNode.GetParameterRegistry().ParameterSupportsInterpolation(OLO_IDENTIFIER("Frequency")));
	
	// Set initial frequency
	EXPECT_FLOAT_EQ(sineNode.GetParameterValue<f32>(OLO_IDENTIFIER("Frequency")), 440.0f);
	
	// Change frequency with interpolation
	sineNode.SetParameterValue(OLO_IDENTIFIER("Frequency"), 880.0f, true);
	
	// Create test buffers
	std::vector<f32> outputBuffer(bufferSize, 0.0f);
	f32* outputs[] = { outputBuffer.data() };
	
	// Process a few frames to test interpolation
	for (int frame = 0; frame < 5; ++frame)
	{
		sineNode.Process(nullptr, outputs, bufferSize);
		
		// Frequency should be interpolating towards 880 Hz
		f32 currentFreq = sineNode.GetParameterValue<f32>(OLO_IDENTIFIER("Frequency"));
		EXPECT_GE(currentFreq, 440.0f);
		EXPECT_LE(currentFreq, 880.0f);
		
		// Output should be generated
		bool hasNonZeroOutput = false;
		for (u32 i = 0; i < bufferSize; ++i)
		{
			if (std::abs(outputBuffer[i]) > 0.001f)
			{
				hasNonZeroOutput = true;
				break;
			}
		}
		EXPECT_TRUE(hasNonZeroOutput);
	}
}

//==============================================================================
/// Test InterpolationUtils
TEST(ParameterInterpolationTest, InterpolationUtils)
{
	f64 sampleRate = 48000.0;
	
	// Test default config
	auto defaultConfig = InterpolationUtils::CreateDefaultConfig(sampleRate);
	EXPECT_EQ(defaultConfig.SampleRate, sampleRate);
	EXPECT_TRUE(defaultConfig.EnableInterpolation);
	EXPECT_EQ(defaultConfig.InterpolationSamples, 480); // 10ms at 48kHz
	
	// Test immediate config
	auto immediateConfig = InterpolationUtils::CreateImmediateConfig();
	EXPECT_FALSE(immediateConfig.EnableInterpolation);
	EXPECT_EQ(immediateConfig.InterpolationSamples, 0);
	
	// Test fast config
	auto fastConfig = InterpolationUtils::CreateFastConfig(sampleRate);
	EXPECT_EQ(fastConfig.SampleRate, sampleRate);
	EXPECT_TRUE(fastConfig.EnableInterpolation);
	EXPECT_EQ(fastConfig.InterpolationSamples, 48); // 1ms at 48kHz
	
	// Test slow config
	auto slowConfig = InterpolationUtils::CreateSlowConfig(sampleRate);
	EXPECT_EQ(slowConfig.SampleRate, sampleRate);
	EXPECT_TRUE(slowConfig.EnableInterpolation);
	EXPECT_EQ(slowConfig.InterpolationSamples, 2400); // 50ms at 48kHz
}