#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Lockstep/LockstepManager.h"
#include "OloEngine/Networking/Lockstep/StateHash.h"
#include "OloEngine/Scene/Scene.h"

using namespace OloEngine;

class LockstepManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = CreateScope<Scene>();
        m_Manager = LockstepManager();
        m_Manager.SetPeers({ 1, 2 });
        m_AppliedInputs.clear();

        m_Manager.SetInputApplyCallback(
            [this](Scene& /*s*/, u32 peerID, const u8* data, u32 size)
            {
                m_AppliedInputs.emplace_back(peerID, std::vector<u8>(data, data + size));
            });
    }

    Scope<Scene> m_Scene;
    LockstepManager m_Manager;
    std::vector<std::pair<u32, std::vector<u8>>> m_AppliedInputs;
};

TEST_F(LockstepManagerTest, AdvanceWaitsForAllPeers)
{
    // Submit input from peer 1 only
    m_Manager.ReceiveInput(1, 1, { 0x01 });

    // Should not advance — peer 2 hasn't submitted
    EXPECT_FALSE(m_Manager.AdvanceTick(*m_Scene));
    EXPECT_EQ(m_Manager.GetCurrentTick(), 0u);

    // Submit input from peer 2
    m_Manager.ReceiveInput(2, 1, { 0x02 });

    // Now should advance
    EXPECT_TRUE(m_Manager.AdvanceTick(*m_Scene));
    EXPECT_EQ(m_Manager.GetCurrentTick(), 1u);
}

TEST_F(LockstepManagerTest, AllInputsAppliedOnAdvance)
{
    m_Manager.ReceiveInput(1, 1, { 0xAA });
    m_Manager.ReceiveInput(2, 1, { 0xBB });
    m_Manager.AdvanceTick(*m_Scene);

    ASSERT_EQ(m_AppliedInputs.size(), 2u);
}

TEST_F(LockstepManagerTest, InputDelaySchedulesFutureTick)
{
    m_Manager.SetInputDelay(2);

    // SubmitInput should target tick 0 + 2 = 2
    m_Manager.SubmitInput(1, { 0x01 });

    EXPECT_FALSE(m_Manager.HasAllInputsForTick(0));
    EXPECT_FALSE(m_Manager.HasAllInputsForTick(1));
    // Tick 2 should have peer 1's input
    EXPECT_FALSE(m_Manager.HasAllInputsForTick(2)); // Still missing peer 2
    m_Manager.ReceiveInput(2, 2, { 0x02 });
    EXPECT_TRUE(m_Manager.HasAllInputsForTick(2));
}

TEST_F(LockstepManagerTest, TickRateGetSet)
{
    EXPECT_EQ(m_Manager.GetTickRate(), 10u);
    m_Manager.SetTickRate(30);
    EXPECT_EQ(m_Manager.GetTickRate(), 30u);
}

TEST_F(LockstepManagerTest, InputDelayGetSet)
{
    EXPECT_EQ(m_Manager.GetInputDelay(), 2u);
    m_Manager.SetInputDelay(5);
    EXPECT_EQ(m_Manager.GetInputDelay(), 5u);
}

TEST(StateHashTest, DeterministicHash)
{
    std::vector<u8> data = { 1, 2, 3, 4, 5 };
    u32 hash1 = StateHash::Compute(data);
    u32 hash2 = StateHash::Compute(data);
    EXPECT_EQ(hash1, hash2);
}

TEST(StateHashTest, DifferentDataDifferentHash)
{
    std::vector<u8> data1 = { 1, 2, 3 };
    std::vector<u8> data2 = { 1, 2, 4 };
    EXPECT_NE(StateHash::Compute(data1), StateHash::Compute(data2));
}

TEST_F(LockstepManagerTest, DesyncDetection)
{
    // Advance to tick 60 (hash check interval) by filling inputs and advancing
    m_Manager.SetHashCheckInterval(1); // Check every tick for testing

    m_Manager.ReceiveInput(1, 1, { 0x01 });
    m_Manager.ReceiveInput(2, 1, { 0x02 });
    m_Manager.AdvanceTick(*m_Scene);

    // Record local state hash
    std::vector<u8> stateData = { 0xAA, 0xBB, 0xCC };
    m_Manager.RecordStateHash(stateData);

    // Compare with matching hash
    u32 localHash = StateHash::Compute(stateData);
    EXPECT_TRUE(m_Manager.CompareRemoteHash(2, 1, localHash));
    EXPECT_FALSE(m_Manager.IsDesynced());

    // Compare with mismatching hash
    EXPECT_FALSE(m_Manager.CompareRemoteHash(2, 1, localHash + 1));
    EXPECT_TRUE(m_Manager.IsDesynced());

    // Clear desync
    m_Manager.ClearDesync();
    EXPECT_FALSE(m_Manager.IsDesynced());
}

TEST_F(LockstepManagerTest, WaitForSlowPeer)
{
    // Peer 1 submits inputs for ticks 1-3
    m_Manager.ReceiveInput(1, 1, { 0x01 });
    m_Manager.ReceiveInput(1, 2, { 0x02 });
    m_Manager.ReceiveInput(1, 3, { 0x03 });

    // Can't advance at all — peer 2 hasn't submitted anything
    EXPECT_FALSE(m_Manager.AdvanceTick(*m_Scene));
    EXPECT_EQ(m_Manager.GetCurrentTick(), 0u);

    // Peer 2 catches up
    m_Manager.ReceiveInput(2, 1, { 0x11 });
    EXPECT_TRUE(m_Manager.AdvanceTick(*m_Scene));
    EXPECT_EQ(m_Manager.GetCurrentTick(), 1u);

    m_Manager.ReceiveInput(2, 2, { 0x12 });
    EXPECT_TRUE(m_Manager.AdvanceTick(*m_Scene));
    EXPECT_EQ(m_Manager.GetCurrentTick(), 2u);
}
