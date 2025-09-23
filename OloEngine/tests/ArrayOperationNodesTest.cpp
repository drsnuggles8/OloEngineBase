#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Nodes/Get.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GetRandom.h"

using namespace OloEngine;
using namespace OloEngine::Audio::SoundGraph;

//===========================================
// Get<T> Node Tests
//===========================================

class GetNodeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_GetF32 = std::make_unique<GetF32>();
        m_GetI32 = std::make_unique<GetI32>();
        
        m_GetF32->Initialize(48000.0, 512);
        m_GetI32->Initialize(48000.0, 512);
    }

    std::unique_ptr<GetF32> m_GetF32;
    std::unique_ptr<GetI32> m_GetI32;
};

TEST_F(GetNodeTest, TypeIDsAndDisplayNames)
{
    EXPECT_EQ(m_GetF32->GetTypeID(), OLO_IDENTIFIER("Get_f32"));
    EXPECT_EQ(m_GetI32->GetTypeID(), OLO_IDENTIFIER("Get_i32"));
    
    EXPECT_STREQ(m_GetF32->GetDisplayName(), "Get (f32)");
    EXPECT_STREQ(m_GetI32->GetDisplayName(), "Get (i32)");
}

TEST_F(GetNodeTest, DefaultArrayContents)
{
    // Test that nodes have default test arrays
    EXPECT_EQ(m_GetF32->GetArraySize(), 5);
    EXPECT_EQ(m_GetI32->GetArraySize(), 5);
    
    // Get default arrays to verify contents
    const auto& f32Array = m_GetF32->GetArray();
    const auto& i32Array = m_GetI32->GetArray();
    
    // Check f32 default array [0.0, 0.25, 0.5, 0.75, 1.0]
    EXPECT_FLOAT_EQ(f32Array[0], 0.0f);
    EXPECT_FLOAT_EQ(f32Array[1], 0.25f);
    EXPECT_FLOAT_EQ(f32Array[2], 0.5f);
    EXPECT_FLOAT_EQ(f32Array[3], 0.75f);
    EXPECT_FLOAT_EQ(f32Array[4], 1.0f);
    
    // Check i32 default array [0, 1, 2, 3, 4]
    EXPECT_EQ(i32Array[0], 0);
    EXPECT_EQ(i32Array[1], 1);
    EXPECT_EQ(i32Array[2], 2);
    EXPECT_EQ(i32Array[3], 3);
    EXPECT_EQ(i32Array[4], 4);
}

TEST_F(GetNodeTest, BasicIndexAccess)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test accessing different indices
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 0.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 0.0f);
    
    // Test index 2
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 2.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 0.5f);
    
    // Test last index
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 4.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 1.0f);
}

TEST_F(GetNodeTest, ModuloWraparound)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test out-of-bounds positive index (should wrap around)
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 7.0f); // 7 % 5 = 2
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 0.5f); // Should get array[2]
    
    // Test negative index (should wrap around correctly)
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), -1.0f); // -1 should become 4
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 1.0f); // Should get array[4]
    
    // Test larger negative index
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), -6.0f); // -6 should become 4 (-6 % 5 = -1, then +5 = 4)
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 1.0f); // Should get array[4]
}

TEST_F(GetNodeTest, TriggerParameterReset)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Set trigger and verify it gets reset after processing
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    
    f32 triggerBefore = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));
    EXPECT_FLOAT_EQ(triggerBefore, 1.0f);
    
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 triggerAfter = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Trigger"));
    EXPECT_FLOAT_EQ(triggerAfter, 0.0f);
}

TEST_F(GetNodeTest, CustomArrayManagement)
{
    // Test setting custom array
    std::vector<f32> customArray = {10.0f, 20.0f, 30.0f};
    m_GetF32->SetArray(customArray);
    
    EXPECT_EQ(m_GetF32->GetArraySize(), 3);
    
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test accessing custom array elements
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 1.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 element = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(element, 20.0f);
    
    // Test adding element
    m_GetF32->AddElement(40.0f);
    EXPECT_EQ(m_GetF32->GetArraySize(), 4);
    
    // Test clearing array
    m_GetF32->ClearArray();
    EXPECT_EQ(m_GetF32->GetArraySize(), 0);
}

TEST_F(GetNodeTest, EmptyArrayHandling)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Clear array and test behavior
    m_GetF32->ClearArray();
    
    f32 elementBefore = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 0.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    // Element should remain unchanged when array is empty
    f32 elementAfter = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(elementBefore, elementAfter);
}

//===========================================
// GetRandom<T> Node Tests
//===========================================

class GetRandomNodeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_GetRandomF32 = std::make_unique<GetRandomF32>();
        m_GetRandomI32 = std::make_unique<GetRandomI32>();
        
        m_GetRandomF32->Initialize(48000.0, 512);
        m_GetRandomI32->Initialize(48000.0, 512);
    }

    std::unique_ptr<GetRandomF32> m_GetRandomF32;
    std::unique_ptr<GetRandomI32> m_GetRandomI32;
};

TEST_F(GetRandomNodeTest, TypeIDsAndDisplayNames)
{
    EXPECT_EQ(m_GetRandomF32->GetTypeID(), OLO_IDENTIFIER("GetRandom_f32"));
    EXPECT_EQ(m_GetRandomI32->GetTypeID(), OLO_IDENTIFIER("GetRandom_i32"));
    
    EXPECT_STREQ(m_GetRandomF32->GetDisplayName(), "Get Random (f32)");
    EXPECT_STREQ(m_GetRandomI32->GetDisplayName(), "Get Random (i32)");
}

TEST_F(GetRandomNodeTest, DefaultArrayContents)
{
	// Test that nodes have default test arrays
	EXPECT_EQ(m_GetRandomF32->GetArraySize(), static_cast<sizet>(5));
	EXPECT_EQ(m_GetRandomI32->GetArraySize(), static_cast<sizet>(5));
	
	// Get default arrays to verify contents
    const auto& f32Array = m_GetRandomF32->GetArray();
    const auto& i32Array = m_GetRandomI32->GetArray();
    
    // Check f32 default array [0.0, 0.25, 0.5, 0.75, 1.0]
    EXPECT_FLOAT_EQ(f32Array[0], 0.0f);
    EXPECT_FLOAT_EQ(f32Array[1], 0.25f);
    EXPECT_FLOAT_EQ(f32Array[2], 0.5f);
    EXPECT_FLOAT_EQ(f32Array[3], 0.75f);
    EXPECT_FLOAT_EQ(f32Array[4], 1.0f);
    
    // Check i32 default array [0, 1, 2, 3, 4]
    EXPECT_EQ(i32Array[0], 0);
    EXPECT_EQ(i32Array[1], 1);
    EXPECT_EQ(i32Array[2], 2);
    EXPECT_EQ(i32Array[3], 3);
    EXPECT_EQ(i32Array[4], 4);
}

TEST_F(GetRandomNodeTest, BasicRandomSelection)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Set a fixed seed for predictable testing
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Seed"), 42);
    
    // Reset to initialize seed
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    // Trigger random selection
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 selectedElement = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    // Selected element should be one of the array values
    const auto& array = m_GetRandomF32->GetArray();
    bool foundInArray = false;
    for (const auto& value : array)
    {
        if (std::abs(selectedElement - value) < 1e-6f)
        {
            foundInArray = true;
            break;
        }
    }
    EXPECT_TRUE(foundInArray);
}

TEST_F(GetRandomNodeTest, SeedConsistency)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test that same seed produces same sequence
    const i32 testSeed = 123;
    
    // First run with seed
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Seed"), testSeed);
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    std::vector<f32> firstSequence;
    for (int i = 0; i < 5; ++i)
    {
        m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
        m_GetRandomF32->Process(inputs, outputs, 64);
        firstSequence.push_back(m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected")));
    }
    
    // Second run with same seed
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Seed"), testSeed);
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    std::vector<f32> secondSequence;
    for (int i = 0; i < 5; ++i)
    {
        m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
        m_GetRandomF32->Process(inputs, outputs, 64);
        secondSequence.push_back(m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected")));
    }
    
    // Sequences should be identical
    EXPECT_EQ(firstSequence.size(), secondSequence.size());
    for (size_t i = 0; i < firstSequence.size(); ++i)
    {
        EXPECT_FLOAT_EQ(firstSequence[i], secondSequence[i]);
    }
}

TEST_F(GetRandomNodeTest, NoRepeatsMode)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Set up a larger array to reduce chance of infinite loops
    std::vector<f32> testArray = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    m_GetRandomF32->SetArray(testArray);
    
    // Enable NoRepeats mode
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("NoRepeats"), 1.0f);
    
    // Get first selection
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    f32 firstSelection = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    // Verify first selection is from our array
    bool firstValid = false;
    for (const auto& value : testArray)
    {
        if (std::abs(firstSelection - value) < 1e-6f)
        {
            firstValid = true;
            break;
        }
    }
    EXPECT_TRUE(firstValid);
    
    // Get second selection - should be different from first
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    f32 secondSelection = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    // Verify second selection is from our array
    bool secondValid = false;
    for (const auto& value : testArray)
    {
        if (std::abs(secondSelection - value) < 1e-6f)
        {
            secondValid = true;
            break;
        }
    }
    EXPECT_TRUE(secondValid);
    
    // NoRepeats should ensure they're different (high probability with 5 elements)
    EXPECT_NE(firstSelection, secondSelection);
}

TEST_F(GetRandomNodeTest, TriggerParameterReset)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test Next trigger reset
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    
    f32 nextBefore = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Next"));
    EXPECT_FLOAT_EQ(nextBefore, 1.0f);
    
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 nextAfter = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Next"));
    EXPECT_FLOAT_EQ(nextAfter, 0.0f);
    
    // Test Reset trigger reset
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    
    f32 resetBefore = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));
    EXPECT_FLOAT_EQ(resetBefore, 1.0f);
    
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 resetAfter = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Reset"));
    EXPECT_FLOAT_EQ(resetAfter, 0.0f);
}

TEST_F(GetRandomNodeTest, CustomArrayManagement)
{
    // Test setting custom array
    std::vector<f32> customArray = {100.0f, 200.0f, 300.0f};
    m_GetRandomF32->SetArray(customArray);
    
    EXPECT_EQ(m_GetRandomF32->GetArraySize(), static_cast<sizet>(3));
    
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test selecting from custom array
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 selectedElement = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    // Selected element should be one of the custom array values
    bool foundInCustomArray = (selectedElement == 100.0f || 
                              selectedElement == 200.0f || 
                              selectedElement == 300.0f);
    EXPECT_TRUE(foundInCustomArray);
    
    // Test adding element
    m_GetRandomF32->AddElement(400.0f);
    EXPECT_EQ(m_GetRandomF32->GetArraySize(), static_cast<sizet>(4));
    
    // Test clearing array
    m_GetRandomF32->ClearArray();
    EXPECT_EQ(m_GetRandomF32->GetArraySize(), static_cast<sizet>(0));
}

TEST_F(GetRandomNodeTest, EmptyArrayHandling)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Clear array and test behavior
    m_GetRandomF32->ClearArray();
    
    f32 selectedBefore = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    // Selected should remain unchanged when array is empty
    f32 selectedAfter = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    EXPECT_FLOAT_EQ(selectedBefore, selectedAfter);
}

TEST_F(GetRandomNodeTest, IntegerTypeSupport)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test i32 version works correctly
    m_GetRandomI32->SetParameterValue(OLO_IDENTIFIER("Seed"), 42);
    m_GetRandomI32->SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
    m_GetRandomI32->Process(inputs, outputs, 64);
    
    m_GetRandomI32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomI32->Process(inputs, outputs, 64);
    
    i32 selectedElement = m_GetRandomI32->GetParameterValue<i32>(OLO_IDENTIFIER("Selected"));
    
    // Selected element should be one of the default i32 array values [0, 1, 2, 3, 4]
    EXPECT_GE(selectedElement, 0);
    EXPECT_LE(selectedElement, 4);
}

//===========================================
// Integration Tests
//===========================================

class ArrayOperationIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_GetF32 = std::make_unique<GetF32>();
        m_GetRandomF32 = std::make_unique<GetRandomF32>();
        
        m_GetF32->Initialize(48000.0, 512);
        m_GetRandomF32->Initialize(48000.0, 512);
    }

    std::unique_ptr<GetF32> m_GetF32;
    std::unique_ptr<GetRandomF32> m_GetRandomF32;
};

TEST_F(ArrayOperationIntegrationTest, GetAndGetRandomComparison)
{
    // Set up same custom array for both nodes
    std::vector<f32> testArray = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
    m_GetF32->SetArray(testArray);
    m_GetRandomF32->SetArray(testArray);
    
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test Get node with specific index
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 2.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 getElement = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(getElement, 30.0f);
    
    // Test GetRandom node produces valid array element
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 randomElement = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    
    bool foundInArray = false;
    for (const auto& value : testArray)
    {
        if (std::abs(randomElement - value) < 1e-6f)
        {
            foundInArray = true;
            break;
        }
    }
    EXPECT_TRUE(foundInArray);
}

TEST_F(ArrayOperationIntegrationTest, ParameterVsOutputConsistency)
{
    f32* inputs[] = {nullptr};
    f32* outputs[] = {nullptr};
    
    // Test Get node - Output and Element parameters should be identical
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Index"), 1.0f);
    m_GetF32->SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
    m_GetF32->Process(inputs, outputs, 64);
    
    f32 getOutput = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
    f32 getElement = m_GetF32->GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
    EXPECT_FLOAT_EQ(getOutput, getElement);
    
    // Test GetRandom node - Output and Selected parameters should be identical
    m_GetRandomF32->SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
    m_GetRandomF32->Process(inputs, outputs, 64);
    
    f32 randomOutput = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
    f32 randomSelected = m_GetRandomF32->GetParameterValue<f32>(OLO_IDENTIFIER("Selected"));
    EXPECT_FLOAT_EQ(randomOutput, randomSelected);
}

TEST_F(ArrayOperationIntegrationTest, NodeUniqueIdentifiers)
{
    // Ensure all node types have unique identifiers
    Identifier getF32ID = m_GetF32->GetTypeID();
    Identifier getRandomF32ID = m_GetRandomF32->GetTypeID();
    
    EXPECT_NE(getF32ID, getRandomF32ID);
    
    // Test display names are different
    EXPECT_STRNE(m_GetF32->GetDisplayName(), m_GetRandomF32->GetDisplayName());
}