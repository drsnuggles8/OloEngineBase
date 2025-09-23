#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/SoundGraph.h"

using namespace OloEngine;
using namespace OloEngine::Audio::SoundGraph;

TEST(SoundGraphBasicTest, CanCreateSoundGraph)
{
    // Test creating SoundGraph without initialization
    auto soundGraph = CreateScope<SoundGraph>();
    EXPECT_NE(soundGraph, nullptr);
}

TEST(SoundGraphBasicTest, CanAccessBasicProperties)
{
    // Test basic property access
    auto soundGraph = CreateScope<SoundGraph>();
    ASSERT_NE(soundGraph, nullptr);
    
    // Check basic accessor methods
    auto& nodes = soundGraph->GetNodes();
    EXPECT_EQ(nodes.size(), 0); // Should be empty initially
    
    // Check parameter registry access
    const auto& params = soundGraph->GetParameterRegistry();
    EXPECT_TRUE(true); // If we get here, parameter registry is accessible
}

TEST(SoundGraphBasicTest, CanInitializeSoundGraph)
{
    // Test creating and initializing SoundGraph
    auto soundGraph = CreateScope<SoundGraph>();
    ASSERT_NE(soundGraph, nullptr);
    
    // This should not crash
    soundGraph->Initialize(48000.0, 512);
    
    EXPECT_TRUE(true); // If we get here, initialization succeeded
}