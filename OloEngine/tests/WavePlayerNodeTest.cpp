#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayerNode.h"
#include "OloEngine/Audio/AudioLoader.h"

using namespace OloEngine::Audio::SoundGraph;
using namespace OloEngine::Audio;

//==============================================================================
/// WavePlayerNode Test Fixture
class WavePlayerNodeTest : public ::testing::Test 
{
protected:
    void SetUp() override 
    {
        m_WavePlayer = std::make_unique<WavePlayerNode>();
        m_SampleRate = 44100.0;
        m_BufferSize = 512;
        m_WavePlayer->Initialize(m_SampleRate, m_BufferSize);
        
        // Create test audio data - simple sine wave
        CreateTestAudioData();
        
        // Setup output buffers
        m_OutputLeft.resize(m_BufferSize);
        m_OutputRight.resize(m_BufferSize);
        m_OutputBuffers[0] = m_OutputLeft.data();
        m_OutputBuffers[1] = m_OutputRight.data();
    }

    void TearDown() override 
    {
        m_WavePlayer.reset();
    }

    void CreateTestAudioData()
    {
        // Create 2 seconds of test audio (sine wave at 440Hz)
        const u32 numFrames = static_cast<u32>(m_SampleRate * 2.0); // 2 seconds
        const u32 numChannels = 2; // Stereo
        const f32 frequency = 440.0f; // A4 note
        
        std::vector<f32> audioData(numFrames * numChannels);
        
        for (u32 frame = 0; frame < numFrames; ++frame)
        {
            f32 time = static_cast<f32>(frame) / static_cast<f32>(m_SampleRate);
            f32 sample = std::sin(2.0f * 3.14159f * frequency * time) * 0.5f;
            
            // Stereo - same signal on both channels
            audioData[frame * numChannels + 0] = sample; // Left
            audioData[frame * numChannels + 1] = sample; // Right
        }
        
        m_WavePlayer->SetAudioData(audioData.data(), numFrames, numChannels);
    }

    void ProcessSamples(u32 numSamples = 0)
    {
        if (numSamples == 0) numSamples = m_BufferSize;
        
        // Clear output buffers
        std::fill(m_OutputLeft.begin(), m_OutputLeft.end(), 0.0f);
        std::fill(m_OutputRight.begin(), m_OutputRight.end(), 0.0f);
        
        m_WavePlayer->Process(nullptr, m_OutputBuffers, numSamples);
    }

    void TriggerPlay()
    {
        // Trigger the Play event properly through the parameter system
        m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Play"), 1.0f);
        ProcessSamples(1); // Process a small buffer to handle the event
    }

    void TriggerStop()
    {
        // Trigger the Stop event properly through the parameter system
        m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Stop"), 1.0f);
        ProcessSamples(1); // Process a small buffer to handle the event
    }

    std::unique_ptr<WavePlayerNode> m_WavePlayer;
    f64 m_SampleRate;
    u32 m_BufferSize;
    
    std::vector<f32> m_OutputLeft;
    std::vector<f32> m_OutputRight;
    f32* m_OutputBuffers[2];
};

//==============================================================================
/// Basic Functionality Tests

TEST_F(WavePlayerNodeTest, Construction) 
{
    EXPECT_NE(m_WavePlayer, nullptr);
    EXPECT_EQ(m_WavePlayer->GetTypeID(), OLO_IDENTIFIER("WavePlayer"));
    EXPECT_STREQ(m_WavePlayer->GetDisplayName(), "Wave Player");
}

TEST_F(WavePlayerNodeTest, InitialState) 
{
    EXPECT_FALSE(m_WavePlayer->IsPlaying());
    EXPECT_FALSE(m_WavePlayer->IsPaused());
    EXPECT_EQ(m_WavePlayer->GetPlaybackPosition(), 0.0);
    EXPECT_EQ(m_WavePlayer->GetCurrentLoopCount(), 0);
    EXPECT_EQ(m_WavePlayer->GetMaxLoopCount(), -1); // Default infinite loops
    EXPECT_FALSE(m_WavePlayer->IsLooping());
}

TEST_F(WavePlayerNodeTest, ParameterAccess) 
{
    // Test parameter defaults
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 0.0f), 1.0f);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), -1.0), 0.0);
    EXPECT_FALSE(m_WavePlayer->GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), true));
    EXPECT_EQ(m_WavePlayer->GetParameterValue<i32>(OLO_IDENTIFIER("LoopCount"), 0), -1);
    
    // Test parameter setting
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Volume"), 0.5f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 0.0f), 0.5f);
    
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Loop"), true);
    EXPECT_TRUE(m_WavePlayer->GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), false));
}

TEST_F(WavePlayerNodeTest, AudioDataSetting) 
{
    EXPECT_GT(m_WavePlayer->GetDuration(), 0.0);
    EXPECT_NEAR(m_WavePlayer->GetDuration(), 2.0, 0.01); // Should be ~2 seconds
}

//==============================================================================
/// Playback Tests

TEST_F(WavePlayerNodeTest, SilentWhenNotPlaying) 
{
    ProcessSamples();
    
    // Should output silence when not playing
    for (u32 i = 0; i < m_BufferSize; ++i)
    {
        EXPECT_FLOAT_EQ(m_OutputLeft[i], 0.0f);
        EXPECT_FLOAT_EQ(m_OutputRight[i], 0.0f);
    }
}

TEST_F(WavePlayerNodeTest, VolumeControl) 
{
    // Test different volume levels (without starting playback)
    std::vector<f32> volumes = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    
    for (f32 volume : volumes)
    {
        m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Volume"), volume);
        ProcessSamples(64); // Process smaller buffer for quicker test
        
        // When not playing, should output silence regardless of volume
        f32 maxOutput = 0.0f;
        for (u32 i = 0; i < 64; ++i)
        {
            maxOutput = std::max(maxOutput, std::abs(m_OutputLeft[i]));
        }
        
        // Should be silent when not playing
        EXPECT_FLOAT_EQ(maxOutput, 0.0f);
    }
}

//==============================================================================
/// Loop Functionality Tests

TEST_F(WavePlayerNodeTest, BasicLooping) 
{
    // Enable looping with 2 loops
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Loop"), true);
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("LoopCount"), 2);
    
    EXPECT_TRUE(m_WavePlayer->IsLooping());
    EXPECT_EQ(m_WavePlayer->GetMaxLoopCount(), 2);
}

TEST_F(WavePlayerNodeTest, InfiniteLooping) 
{
    // Enable infinite looping
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Loop"), true);
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("LoopCount"), -1);
    
    EXPECT_TRUE(m_WavePlayer->IsLooping());
    EXPECT_EQ(m_WavePlayer->GetMaxLoopCount(), -1);
}

TEST_F(WavePlayerNodeTest, LoopCountTracking) 
{
    // Test that loop count is properly tracked
    EXPECT_EQ(m_WavePlayer->GetCurrentLoopCount(), 0);
    
    // Simulate playback through multiple loops
    // Note: This would require triggering the actual loop logic
    // For now, test the getter/setter functionality
    m_WavePlayer->SetMaxLoopCount(3);
    EXPECT_EQ(m_WavePlayer->GetMaxLoopCount(), 3);
}

//==============================================================================
/// Parameter Tests

TEST_F(WavePlayerNodeTest, PitchParameter) 
{
    // Test pitch parameter
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Pitch"), 2.0f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 0.0f), 2.0f);
    
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Pitch"), 0.5f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 0.0f), 0.5f);
}

TEST_F(WavePlayerNodeTest, StartTimeParameter) 
{
    // Test start time parameter
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("StartTime"), 0.5);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), 0.0), 0.5);
}

TEST_F(WavePlayerNodeTest, LoopPositionParameters) 
{
    // Test loop start/end position parameters
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("LoopStart"), 0.25);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("LoopStart"), 0.0), 0.25);
    
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("LoopEnd"), 1.5);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("LoopEnd"), 0.0), 1.5);
}

//==============================================================================
/// Output Parameter Tests

TEST_F(WavePlayerNodeTest, OutputParameters) 
{
    ProcessSamples();
    
    // Test output parameters exist and are accessible
    f32 outLeft = m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("OutLeft"), -999.0f);
    f32 outRight = m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("OutRight"), -999.0f);
    f32 playbackPos = m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("PlaybackPosition"), -999.0f);
    i32 loopCount = m_WavePlayer->GetParameterValue<i32>(OLO_IDENTIFIER("CurrentLoopCount"), -999);
    
    EXPECT_NE(outLeft, -999.0f);
    EXPECT_NE(outRight, -999.0f);
    EXPECT_NE(playbackPos, -999.0f);
    EXPECT_NE(loopCount, -999);
    
    // When not playing, outputs should be 0
    EXPECT_FLOAT_EQ(outLeft, 0.0f);
    EXPECT_FLOAT_EQ(outRight, 0.0f);
    EXPECT_FLOAT_EQ(playbackPos, 0.0f);
    EXPECT_EQ(loopCount, 0);
}

//==============================================================================
/// Setter Method Tests

TEST_F(WavePlayerNodeTest, SetterMethods) 
{
    // Test all setter methods
    m_WavePlayer->SetVolume(0.7f);
    m_WavePlayer->SetPitch(1.5f);
    m_WavePlayer->SetLoop(true);
    m_WavePlayer->SetMaxLoopCount(5);
    m_WavePlayer->SetStartTime(0.3);
    m_WavePlayer->SetLoopStart(0.1);
    m_WavePlayer->SetLoopEnd(1.8);
    
    // Verify setters work through getters
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 0.0f), 0.7f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 0.0f), 1.5f);
    EXPECT_TRUE(m_WavePlayer->GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), false));
    EXPECT_EQ(m_WavePlayer->GetParameterValue<i32>(OLO_IDENTIFIER("LoopCount"), 0), 5);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), 0.0), 0.3);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("LoopStart"), 0.0), 0.1);
    EXPECT_DOUBLE_EQ(m_WavePlayer->GetParameterValue<f64>(OLO_IDENTIFIER("LoopEnd"), 0.0), 1.8);
}

//==============================================================================
/// Edge Case Tests

TEST_F(WavePlayerNodeTest, ZeroVolumeProcessing) 
{
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Volume"), 0.0f);
    ProcessSamples();
    
    // Should output silence with zero volume
    for (u32 i = 0; i < m_BufferSize; ++i)
    {
        EXPECT_FLOAT_EQ(m_OutputLeft[i], 0.0f);
        EXPECT_FLOAT_EQ(m_OutputRight[i], 0.0f);
    }
}

TEST_F(WavePlayerNodeTest, ExtremeParameters) 
{
    // Test extreme but valid parameter values
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Pitch"), 0.1f); // Very slow
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("Volume"), 2.0f); // Loud
    m_WavePlayer->SetParameterValue(OLO_IDENTIFIER("LoopCount"), 1000); // Many loops
    
    // Should not crash
    ProcessSamples();
    
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 0.0f), 0.1f);
    EXPECT_FLOAT_EQ(m_WavePlayer->GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 0.0f), 2.0f);
    EXPECT_EQ(m_WavePlayer->GetParameterValue<i32>(OLO_IDENTIFIER("LoopCount"), 0), 1000);
}

TEST_F(WavePlayerNodeTest, NoAudioData) 
{
    // Create a new WavePlayer without audio data
    auto emptyPlayer = std::make_unique<WavePlayerNode>();
    emptyPlayer->Initialize(m_SampleRate, m_BufferSize);
    
    std::vector<f32> emptyOutputLeft(m_BufferSize);
    std::vector<f32> emptyOutputRight(m_BufferSize);
    f32* emptyOutputs[2] = { emptyOutputLeft.data(), emptyOutputRight.data() };
    
    // Should handle gracefully and output silence
    emptyPlayer->Process(nullptr, emptyOutputs, m_BufferSize);
    
    for (u32 i = 0; i < m_BufferSize; ++i)
    {
        EXPECT_FLOAT_EQ(emptyOutputLeft[i], 0.0f);
        EXPECT_FLOAT_EQ(emptyOutputRight[i], 0.0f);
    }
}

//==============================================================================
/// Performance Tests

TEST_F(WavePlayerNodeTest, ProcessingPerformance) 
{
    // Simple performance test - should complete quickly
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000; ++i)
    {
        ProcessSamples();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should process 1000 buffers in reasonable time (less than 1 second)
    EXPECT_LT(duration.count(), 1000);
}