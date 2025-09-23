#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Nodes/RepeatTrigger.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriggerCounter.h"
#include "OloEngine/Audio/SoundGraph/Nodes/DelayedTrigger.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GetRandom.h"
#include "OloEngine/Audio/SoundGraph/Nodes/Get.h"

using namespace OloEngine;
using namespace OloEngine::Audio::SoundGraph;

//===========================================
// RepeatTrigger Tests
//===========================================

class RepeatTriggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_RepeatTrigger = std::make_unique<RepeatTrigger>();
        m_RepeatTrigger->Initialize(48000.0, 512);
    }

    std::unique_ptr<RepeatTrigger> m_RepeatTrigger;
};

TEST_F(RepeatTriggerTest, ParameterTriggeringTest)
{
    // Test parameter-based triggering
    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);
    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Period"), 0.5f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};

    m_RepeatTrigger->Process(inputs, outputs, 64);

    // Check if playing state is set
    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));
    EXPECT_GT(isPlaying, 0.5f);

    // Check if start parameter was reset
    f32 startValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Start"));
    EXPECT_LT(startValue, 0.5f);
}

TEST_F(RepeatTriggerTest, StopTriggeringTest)
{
    // Start the trigger
    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};

    m_RepeatTrigger->Process(inputs, outputs, 64);

    // Verify it's playing
    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));
    EXPECT_GT(isPlaying, 0.5f);

    // Stop the trigger
    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Stop"), 1.0f);
    m_RepeatTrigger->Process(inputs, outputs, 64);

    // Check if playing state is cleared
    isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));
    EXPECT_LT(isPlaying, 0.5f);

    // Check if stop parameter was reset
    f32 stopValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Stop"));
    EXPECT_LT(stopValue, 0.5f);
}

//===========================================
// TriggerCounter Tests
//===========================================

class TriggerCounterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_TriggerCounter = std::make_unique<TriggerCounter>();
        m_TriggerCounter->Initialize(48000.0, 512);
    }

    std::unique_ptr<TriggerCounter> m_TriggerCounter;
};

TEST_F(TriggerCounterTest, BasicCountingTest)
{
    // Set parameters
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StartValue"), 10.0f);
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StepSize"), 5.0f);
    
    // Trigger first count
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};

    m_TriggerCounter->Process(inputs, outputs, 64);

    // Check count and value
    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));
    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Value"));

    EXPECT_EQ(count, 1.0f);
    EXPECT_EQ(value, 15.0f); // StartValue + StepSize * Count = 10 + 5 * 1

    // Check trigger parameter was reset
    f32 triggerValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));
    EXPECT_LT(triggerValue, 0.5f);
}

TEST_F(TriggerCounterTest, MultipleTriggersTest)
{
    // Set parameters
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StartValue"), 0.0f);
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StepSize"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};

    // Trigger multiple times
    for (int i = 0; i < 3; ++i)
    {
        m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
        m_TriggerCounter->Process(inputs, outputs, 64);
    }

    // Verify count
    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));
    EXPECT_EQ(count, 3.0f);
}

TEST_F(TriggerCounterTest, ResetTest)
{
    // Trigger a few times first
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};

    m_TriggerCounter->Process(inputs, outputs, 64);

    // Reset
    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_TriggerCounter->Process(inputs, outputs, 64);

    // Check if reset worked
    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));
    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Value"));
    EXPECT_EQ(count, 0.0f);
    EXPECT_EQ(value, 0.0f);

    // Check if reset parameter was reset
    f32 resetValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));
    EXPECT_LT(resetValue, 0.5f);
}

//===========================================
// DelayedTrigger Tests
//===========================================

class DelayedTriggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_DelayedTrigger = std::make_unique<DelayedTrigger>();
        m_DelayedTrigger->Initialize(48000.0, 512);
    }

    std::unique_ptr<DelayedTrigger> m_DelayedTrigger;
};

TEST_F(DelayedTriggerTest, ParameterTriggeringTest)
{
    // Test parameter-based triggering
    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("DelayTime"), 0.1f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    m_DelayedTrigger->Process(inputs, outputs, 64);

    // Check if trigger parameter was reset
    f32 triggerValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));
    EXPECT_LT(triggerValue, 0.5f);
}

TEST_F(DelayedTriggerTest, DelayTimeParameterTest)
{
    // Start delay
    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    m_DelayedTrigger->Process(inputs, outputs, 64);

    // Reset the delay
    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_DelayedTrigger->Process(inputs, outputs, 64);

    // Check if reset parameter was reset
    f32 resetValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));
    EXPECT_LT(resetValue, 0.5f);
}

//===========================================
// GetRandom Tests
//===========================================

class GetRandomTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_GetRandom = std::make_unique<GetRandomF32>();
        m_GetRandom->Initialize(48000.0, 512);
    }

    std::unique_ptr<GetRandomF32> m_GetRandom;
};

TEST_F(GetRandomTest, ParameterTriggeringTest)
{
    // Test parameter-based triggering
    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Seed"), 42.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    m_GetRandom->Process(inputs, outputs, 64);

    // Check if trigger parameter was reset
    f32 nextValue = m_GetRandom->GetParameterValue<f32>(OLO_IDENTIFIER("Next"));
    EXPECT_LT(nextValue, 0.5f);
}

TEST_F(GetRandomTest, ResetSeedTest)
{
    // Reset seed
    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    m_GetRandom->Process(inputs, outputs, 64);

    // Check if reset parameter was reset
    f32 resetValue = m_GetRandom->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));
    EXPECT_LT(resetValue, 0.5f);
}

//===========================================
// Get Tests
//===========================================

class GetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Get = std::make_unique<GetF32>();
        m_Get->Initialize(48000.0, 512);
    }

    std::unique_ptr<GetF32> m_Get;
};

TEST_F(GetTest, ParameterTriggeringTest)
{
    // Test parameter-based triggering
    m_Get->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_Get->SetParameterValue(OLO_IDENTIFIER("Index"), 2.0f);

    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    m_Get->Process(inputs, outputs, 64);

    // Check if trigger parameter was reset
    f32 triggerValue = m_Get->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));
    EXPECT_LT(triggerValue, 0.5f);
}

//===========================================
// Integration Tests
//===========================================

class TriggerNodeIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize multiple nodes for integration testing
        m_RepeatTrigger = std::make_unique<RepeatTrigger>();
        m_TriggerCounter = std::make_unique<TriggerCounter>();
        m_GetRandom = std::make_unique<GetRandomF32>();

        m_RepeatTrigger->Initialize(48000.0, 512);
        m_TriggerCounter->Initialize(48000.0, 512);
        m_GetRandom->Initialize(48000.0, 512);
    }

    std::unique_ptr<RepeatTrigger> m_RepeatTrigger;
    std::unique_ptr<TriggerCounter> m_TriggerCounter;
    std::unique_ptr<GetRandomF32> m_GetRandom;
};

TEST_F(TriggerNodeIntegrationTest, NodeTypeIDsTest)
{
    // Test that all nodes have unique type IDs
    Identifier repeatID = m_RepeatTrigger->GetTypeID();
    Identifier counterID = m_TriggerCounter->GetTypeID();
    Identifier randomID = m_GetRandom->GetTypeID();

    EXPECT_NE(repeatID, counterID);
    EXPECT_NE(repeatID, randomID);
    EXPECT_NE(counterID, randomID);
}

TEST_F(TriggerNodeIntegrationTest, DisplayNamesTest)
{
    // Test that all nodes have meaningful display names
    EXPECT_STRNE(m_RepeatTrigger->GetDisplayName(), "");
    EXPECT_STRNE(m_TriggerCounter->GetDisplayName(), "");
    EXPECT_STRNE(m_GetRandom->GetDisplayName(), "");
}
