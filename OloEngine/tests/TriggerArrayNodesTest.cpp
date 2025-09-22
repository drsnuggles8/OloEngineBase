#include "OloEnginePCH.h"#include "OloEnginePCH.h"

#include <gtest/gtest.h>#include <gtest/gtest.h>



#include "OloEngine/Audio/SoundGraph/Nodes/RepeatTrigger.h"#include "OloEngine/Audio/SoundGraph/Nodes/RepeatTrigger.h"

#include "OloEngine/Audio/SoundGraph/Nodes/TriggerCounter.h"#include "OloEngine/Audio/SoundGraph/Nodes/TriggerCounter.h"

#include "OloEngine/Audio/SoundGraph/Nodes/DelayedTrigger.h"#include "OloEngine/Audio/SoundGraph/Nodes/DelayedTrigger.h"

#include "OloEngine/Audio/SoundGraph/Nodes/GetRandom.h"#include "OloEngine/Audio/SoundGraph/Nodes/GetRandom.h"

#include "OloEngine/Audio/SoundGraph/Nodes/Get.h"#include "OloEngine/Audio/SoundGraph/Nodes/Get.h"



using namespace OloEngine;using namespace OloEngine;

using namespace OloEngine::Audio::SoundGraph;using namespace OloEngine::Audio::SoundGraph;



//===========================================//===========================================

// RepeatTrigger Tests// RepeatTrigger Tests

//===========================================//===========================================



class RepeatTriggerTest : public ::testing::Test class RepeatTriggerTest : public ::testing::Test 

{{

protected:protected:

    void SetUp() override     void SetUp() override 

    {    {

        m_RepeatTrigger = std::make_unique<RepeatTrigger>();        m_RepeatTrigger = std::make_unique<RepeatTrigger>();

        m_RepeatTrigger->Initialize(44100.0, 512);        m_RepeatTrigger->Initialize(48000.0, 512);

    }    }

    

    std::unique_ptr<RepeatTrigger> m_RepeatTrigger;    std::unique_ptr<RepeatTrigger> m_RepeatTrigger;

};};



TEST_F(RepeatTriggerTest, ParameterTriggeringTest)TEST_F(RepeatTriggerTest, ParameterTriggeringTest)

{{

    // Test parameter-based triggering    // Test parameter-based triggering

    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);

    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Period"), 0.5f);    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Period"), 0.5f);

        

    f32* inputs[] = {nullptr};    f32* inputs[] = {nullptr};

    f32* outputs[] = {nullptr};    f32* outputs[] = {nullptr};

    m_RepeatTrigger->Process(inputs, outputs, 64);    m_RepeatTrigger->Process(inputs, outputs, 64);

        

    // Check if playing state is set    // Check if playing state is set

    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));

    EXPECT_GT(isPlaying, 0.5f);    EXPECT_GT(isPlaying, 0.5f);

        

    // Check if start parameter was reset      // Check if start parameter was reset

    f32 startValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Start"));    f32 startValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Start"));

    EXPECT_LT(startValue, 0.5f);    EXPECT_LT(startValue, 0.5f);

}}



TEST_F(RepeatTriggerTest, StopTriggeringTest)TEST_F(RepeatTriggerTest, StopTriggeringTest)

{{

    // Start the trigger    // Start the trigger

    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);

        

    f32* inputs[] = {nullptr};    f32* inputs[] = {nullptr};

    f32* outputs[] = {nullptr};    f32* outputs[] = {nullptr};

    m_RepeatTrigger->Process(inputs, outputs, 64);    m_RepeatTrigger->Process(inputs, outputs, 64);

        

    // Verify it's playing    // Verify it's playing

    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));    f32 isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));

    EXPECT_GT(isPlaying, 0.5f);    EXPECT_GT(isPlaying, 0.5f);

        

    // Stop the trigger    // Stop the trigger

    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Stop"), 1.0f);    m_RepeatTrigger->SetParameterValue(OLO_IDENTIFIER("Stop"), 1.0f);

    m_RepeatTrigger->Process(inputs, outputs, 64);    m_RepeatTrigger->Process(inputs, outputs, 64);

        

    // Check if playing state is cleared    // Check if playing state is cleared

    isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));    isPlaying = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying"));

    EXPECT_LT(isPlaying, 0.5f);    EXPECT_LT(isPlaying, 0.5f);

        

    // Check if stop parameter was reset    // Check if stop parameter was reset

    f32 stopValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Stop"));    f32 stopValue = m_RepeatTrigger->GetParameterValue<f32>(OLO_IDENTIFIER(\"Stop\"));

    EXPECT_LT(stopValue, 0.5f);    EXPECT_LT(stopValue, 0.5f);

}}



//===========================================//===========================================

// TriggerCounter Tests// TriggerCounter Tests

//===========================================//===========================================



class TriggerCounterTest : public ::testing::Test class TriggerCounterTest : public ::testing::Test 

{{

protected:protected:

    void SetUp() override     void SetUp() override 

    {    {

        m_TriggerCounter = std::make_unique<TriggerCounter>();        m_TriggerCounter = std::make_unique<TriggerCounter>();

        m_TriggerCounter->Initialize(44100.0, 512);        m_TriggerCounter->Initialize(48000.0, 512);

    }    }

    

    std::unique_ptr<TriggerCounter> m_TriggerCounter;    std::unique_ptr<TriggerCounter> m_TriggerCounter;

};};



TEST_F(TriggerCounterTest, BasicCountingTest)TEST_F(TriggerCounterTest, ParameterTriggeringTest)

{{

    // First trigger    // Test parameter-based triggering

    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER(\"Trigger\"), 1.0f);

    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StartValue"), 10.0f);    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER(\"StartValue\"), 10.0f);

    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("StepSize"), 5.0f);    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER(\"StepSize\"), 5.0f);

        

    f32* inputs[] = {nullptr};    f32* inputs[] = {nullptr};

    f32* outputs[] = {nullptr};    f32* outputs[] = {nullptr};

    m_TriggerCounter->Process(inputs, outputs, 64);    m_TriggerCounter->Process(inputs, outputs, 64);

        

    // Check count and value    // Check count and value

    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Count\"));

    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Value"));    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Value\"));

    EXPECT_EQ(count, 1.0f);    EXPECT_EQ(count, 1.0f);

    EXPECT_EQ(value, 15.0f); // 10 + 5*1    EXPECT_EQ(value, 15.0f); // StartValue + StepSize * Count = 10 + 5 * 1

        

    // Check trigger parameter was reset    // Check if trigger parameter was reset

    f32 triggerValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));    f32 triggerValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Trigger\"));

    EXPECT_LT(triggerValue, 0.5f);    EXPECT_LT(triggerValue, 0.5f);

}}



TEST_F(TriggerCounterTest, MultipleTriggersTest)TEST_F(TriggerCounterTest, ResetTest)

{{

    // Second trigger     // Trigger a few times

    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);    for (int i = 0; i < 3; ++i)

        {

    f32* inputs[] = {nullptr};        m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER(\"Trigger\"), 1.0f);

    f32* outputs[] = {nullptr};        

    m_TriggerCounter->Process(inputs, outputs, 64);        f32* inputs[] = {nullptr};

            f32* outputs[] = {nullptr};

    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));        m_TriggerCounter->Process(inputs, outputs, 64);

    EXPECT_EQ(count, 1.0f);    }

}    

    // Verify count

TEST_F(TriggerCounterTest, ResetTest)    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Count\"));

{    EXPECT_EQ(count, 3.0f);

    // Set reset trigger    

    m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);    // Reset

        m_TriggerCounter->SetParameterValue(OLO_IDENTIFIER(\"Reset\"), 1.0f);

    f32* inputs[] = {nullptr};    

    f32* outputs[] = {nullptr};    f32* inputs[] = {nullptr};

    m_TriggerCounter->Process(inputs, outputs, 64);    f32* outputs[] = {nullptr};

        m_TriggerCounter->Process(inputs, outputs, 64);

    // Check count and value reset to initial    

    f32 count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Count"));    // Check if reset worked

    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Value"));    count = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Count\"));

    EXPECT_EQ(count, 0.0f);    f32 value = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Value\"));

    EXPECT_EQ(value, 0.0f); // Default StartValue    EXPECT_EQ(count, 0.0f);

        EXPECT_EQ(value, 0.0f);

    // Check reset parameter was reset    

    f32 resetValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));    // Check if reset parameter was reset

    EXPECT_LT(resetValue, 0.5f);    f32 resetValue = m_TriggerCounter->GetParameterValue<f32>(OLO_IDENTIFIER(\"Reset\"));

}    EXPECT_LT(resetValue, 0.5f);

}

//===========================================

// DelayedTrigger Tests  //===========================================

//===========================================// DelayedTrigger Tests

//===========================================

class DelayedTriggerTest : public ::testing::Test 

{class DelayedTriggerTest : public ::testing::Test 

protected:{

    void SetUp() override protected:

    {    void SetUp() override 

        m_DelayedTrigger = std::make_unique<DelayedTrigger>();    {

        m_DelayedTrigger->Initialize(1000.0, 512); // 1kHz for easy timing calculations        m_DelayedTrigger = std::make_unique<DelayedTrigger>();

    }        m_DelayedTrigger->Initialize(48000.0, 512);

        }

    std::unique_ptr<DelayedTrigger> m_DelayedTrigger;

};    std::unique_ptr<DelayedTrigger> m_DelayedTrigger;

};

TEST_F(DelayedTriggerTest, BasicDelayTest)

{TEST_F(DelayedTriggerTest, ParameterTriggeringTest)

    // Set delay parameters{

    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);    // Test parameter-based triggering

    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("DelayTime"), 100.0f); // 100ms = 100 samples at 1kHz    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER(\"Trigger\"), 1.0f);

        m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER(\"DelayTime\"), 0.1f);

    f32* inputs[] = {nullptr};    

    f32* outputs[] = {nullptr};    f32* inputs[] = {nullptr};

        f32* outputs[] = {nullptr};

    // Process delay - should not trigger immediately    m_DelayedTrigger->Process(inputs, outputs, 64);

    m_DelayedTrigger->Process(inputs, outputs, 64);    

        // Check if trigger parameter was reset

    // Check trigger parameter was reset    f32 triggerValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER(\"Trigger\"));

    f32 triggerValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));    EXPECT_LT(triggerValue, 0.5f);

    EXPECT_LT(triggerValue, 0.5f);}

}

TEST_F(DelayedTriggerTest, ResetTest)

TEST_F(DelayedTriggerTest, DelayTimeParameterTest){

{    // Start delay

    // Second delay    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER(\"Trigger\"), 1.0f);

    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);    

        f32* inputs[] = {nullptr};

    f32* inputs[] = {nullptr};    f32* outputs[] = {nullptr};

    f32* outputs[] = {nullptr};    m_DelayedTrigger->Process(inputs, outputs, 64);

    m_DelayedTrigger->Process(inputs, outputs, 64);    

}    // Reset the delay

    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER(\"Reset\"), 1.0f);

TEST_F(DelayedTriggerTest, ResetTest)    m_DelayedTrigger->Process(inputs, outputs, 64);

{    

    // Set reset    // Check if reset parameter was reset

    m_DelayedTrigger->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);    f32 resetValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER(\"Reset\"));

        EXPECT_LT(resetValue, 0.5f);

    f32* inputs[] = {nullptr};}

    f32* outputs[] = {nullptr};

    m_DelayedTrigger->Process(inputs, outputs, 64);//===========================================

    // GetRandom Tests

    // Check reset parameter was reset //===========================================

    f32 resetValue = m_DelayedTrigger->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));

    EXPECT_LT(resetValue, 0.5f);class GetRandomTest : public ::testing::Test 

}{

protected:

//===========================================    void SetUp() override 

// GetRandom Tests    {

//===========================================        m_GetRandom = std::make_unique<GetRandomF32>();

        m_GetRandom->Initialize(48000.0, 512);

class GetRandomTest : public ::testing::Test     }

{

protected:    std::unique_ptr<GetRandomF32> m_GetRandom;

    void SetUp() override };

    {

        m_GetRandom = std::make_unique<GetRandomF32>();TEST_F(GetRandomTest, ParameterTriggeringTest)

        m_GetRandom->Initialize(44100.0, 512);{

            // Test parameter-based triggering

        // Set up test array    m_GetRandom->SetParameterValue(GetRandomF32::Next_ID, 1.0f);

        std::vector<f32> testArray = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};    m_GetRandom->SetParameterValue(GetRandomF32::Seed_ID, 42.0f);

        m_GetRandom->SetArray(testArray);    

    }    f32* inputs[] = {nullptr};

        f32* outputs[] = {nullptr};

    std::unique_ptr<GetRandomF32> m_GetRandom;    m_GetRandom->Process(inputs, outputs, 64);

};    

    // Check if trigger parameter was reset

TEST_F(GetRandomTest, BasicRandomTest)    f32 nextValue = m_GetRandom->GetParameterValue<f32>(GetRandomF32::Next_ID);

{    EXPECT_LT(nextValue, 0.5f);

    // Set seed and trigger}

    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);

    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Seed"), 12345.0f);TEST_F(GetRandomTest, ResetSeedTest)

    {

    f32* inputs[] = {nullptr};    // Reset seed

    f32* outputs[] = {nullptr};    m_GetRandom->SetParameterValue(GetRandomF32::Reset_ID, 1.0f);

    m_GetRandom->Process(inputs, outputs, 64);    

        f32* inputs[] = {nullptr};

    // Check if next parameter was reset    f32* outputs[] = {nullptr};

    f32 nextValue = m_GetRandom->GetParameterValue<f32>(OLO_IDENTIFIER("Next"));    m_GetRandom->Process(inputs, outputs, 64);

    EXPECT_LT(nextValue, 0.5f);    

}    // Check if reset parameter was reset

    f32 resetValue = m_GetRandom->GetParameterValue<f32>(GetRandomF32::Reset_ID);

TEST_F(GetRandomTest, ResetTest)    EXPECT_LT(resetValue, 0.5f);

{}

    // Set reset

    m_GetRandom->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);//===========================================

    // Get Tests

    f32* inputs[] = {nullptr};//===========================================

    f32* outputs[] = {nullptr};

    m_GetRandom->Process(inputs, outputs, 64);class GetTest : public ::testing::Test 

    {

    // Check reset parameter was resetprotected:

    f32 resetValue = m_GetRandom->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));    void SetUp() override 

    EXPECT_LT(resetValue, 0.5f);    {

}        m_Get = std::make_unique<GetF32>();

        m_Get->Initialize(48000.0, 512);

//===========================================    }

// Get Tests

//===========================================    std::unique_ptr<GetF32> m_Get;

};

class GetTest : public ::testing::Test 

{TEST_F(GetTest, ParameterTriggeringTest)

protected:{

    void SetUp() override     // Test parameter-based triggering

    {    m_Get->SetParameterValue(GetF32::Trigger_ID, 1.0f);

        m_Get = std::make_unique<GetF32>();    m_Get->SetParameterValue(GetF32::Index_ID, 2.0f);

        m_Get->Initialize(44100.0, 512);    

            f32* inputs[] = {nullptr};

        // Set up test array    f32* outputs[] = {nullptr};

        std::vector<f32> testArray = {10.0f, 20.0f, 30.0f, 40.0f};    m_Get->Process(inputs, outputs, 64);

        m_Get->SetArray(testArray);    

    }    // Check if trigger parameter was reset

        f32 triggerValue = m_Get->GetParameterValue<f32>(GetF32::Trigger_ID);

    std::unique_ptr<GetF32> m_Get;    EXPECT_LT(triggerValue, 0.5f);

};}



TEST_F(GetTest, BasicIndexingTest)//===========================================

{// Integration Tests

    // Test index access//===========================================

    m_Get->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);

    m_Get->SetParameterValue(OLO_IDENTIFIER("Index"), 0.0f);class TriggerNodeIntegrationTest : public ::testing::Test 

    {

    f32* inputs[] = {nullptr};protected:

    f32* outputs[] = {nullptr};    void SetUp() override 

    m_Get->Process(inputs, outputs, 64);    {

            // Initialize multiple nodes for integration testing

    f32 element = m_Get->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));        m_RepeatTrigger = std::make_unique<RepeatTrigger>();

    EXPECT_NEAR(element, 10.0f, 0.001f);        m_TriggerCounter = std::make_unique<TriggerCounter>();

}        m_GetRandom = std::make_unique<GetRandomF32>();

        

TEST_F(GetTest, IndexWraparoundTest)        m_RepeatTrigger->Initialize(48000.0, 512);

{        m_TriggerCounter->Initialize(48000.0, 512);

    // Test wraparound behavior (index 4 should wrap to 0)        m_GetRandom->Initialize(48000.0, 512);

    m_Get->SetParameterValue(OLO_IDENTIFIER("Index"), 4.0f);    }

    m_Get->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);

        std::unique_ptr<RepeatTrigger> m_RepeatTrigger;

    f32* inputs[] = {nullptr};    std::unique_ptr<TriggerCounter> m_TriggerCounter;

    f32* outputs[] = {nullptr};    std::unique_ptr<GetRandomF32> m_GetRandom;

    m_Get->Process(inputs, outputs, 64);};

    

    f32 element = m_Get->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));TEST_F(TriggerNodeIntegrationTest, NodeTypeIDsTest)

    EXPECT_NEAR(element, 10.0f, 0.001f); // Should be first element{

}    // Test that all nodes have unique type IDs
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
