#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/AudioThread.h"

using namespace OloEngine;
using namespace OloEngine::Audio;

//===========================================
// Simplified AudioThread Tests for Debugging
//===========================================

TEST(AudioThreadDebugTest, StaticInitialization)
{
    // Test that AudioThread can be used without explicit initialization
    EXPECT_FALSE(AudioThread::IsRunning());
    EXPECT_FALSE(AudioThread::IsAudioThread());
    
    // This should not crash
    auto threadID = AudioThread::GetThreadID();
    EXPECT_TRUE(true); // If we get here, basic static access works
}

TEST(AudioThreadDebugTest, StartAndStopBasic)
{
    // Ensure not running initially
    if (AudioThread::IsRunning())
    {
        AudioThread::Stop();
    }
    
    EXPECT_FALSE(AudioThread::IsRunning());
    
    // Try to start
    bool started = AudioThread::Start();
    EXPECT_TRUE(started);
    EXPECT_TRUE(AudioThread::IsRunning());
    
    // Give it a moment to fully start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Stop it
    bool stopped = AudioThread::Stop();
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(AudioThread::IsRunning());
}

TEST(AudioThreadDebugTest, TaskAdditionWithoutExecution)
{
    // Test adding tasks without worrying about execution
    if (!AudioThread::IsRunning())
    {
        AudioThread::Start();
    }
    
    std::atomic<int> counter{0};
    
    // Add a simple task
    AudioThread::ExecuteOnAudioThread([&counter]()
    {
        counter.fetch_add(1);
    }, "TestTask");
    
    // Don't wait for execution, just verify the system didn't crash
    EXPECT_TRUE(AudioThread::IsRunning());
    
    AudioThread::Stop();
}