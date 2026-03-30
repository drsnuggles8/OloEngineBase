#include <gtest/gtest.h>
#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"

using namespace OloEngine::Audio;

class AudioEventsManagerTest : public ::testing::Test
{
  protected:
    AudioCommandRegistry m_Registry;
    AudioEventsManager m_Manager;

    void SetUp() override
    {
        m_Manager.Init(&m_Registry);
    }

    void TearDown() override
    {
        m_Manager.Shutdown();
    }
};

TEST_F(AudioEventsManagerTest, PostTriggerReturnsUniqueIDs)
{
    auto id = m_Registry.AddTrigger("TestEvent");
    auto eid1 = m_Manager.PostTrigger(id);
    auto eid2 = m_Manager.PostTrigger(id);
    EXPECT_NE(eid1, eid2);
    EXPECT_GT(eid1, 0u);
    EXPECT_GT(eid2, 0u);
}

TEST_F(AudioEventsManagerTest, UpdateProcessesPendingEvents)
{
    auto id = m_Registry.AddTrigger("TestEvent");
    // Add a Stop action (no audio file needed, won't crash)
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(id, action);

    auto eid = m_Manager.PostTrigger(id);
    // Before update, event is pending but active tracking starts after Update
    m_Manager.Update(OloEngine::Timestep(0.016f));
    // Stop actions don't create long-lived entries, so should not be active
    EXPECT_FALSE(m_Manager.IsEventActive(eid));
}

TEST_F(AudioEventsManagerTest, PostTriggerWithUnknownCommandStillReturnsID)
{
    CommandID unknown(99999u);
    auto eid = m_Manager.PostTrigger(unknown);
    EXPECT_GT(eid, 0u);
    // Update should handle gracefully (no crash)
    m_Manager.Update(OloEngine::Timestep(0.016f));
}

TEST_F(AudioEventsManagerTest, StopAllClearsActiveEvents)
{
    auto id = m_Registry.AddTrigger("TestEvent");
    m_Manager.PostTrigger(id);
    m_Manager.Update(OloEngine::Timestep(0.016f));
    m_Manager.StopAllEvents();
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}

TEST_F(AudioEventsManagerTest, IsEventActiveReturnsFalseForInvalidID)
{
    EXPECT_FALSE(m_Manager.IsEventActive(0));
    EXPECT_FALSE(m_Manager.IsEventActive(999999));
}

TEST_F(AudioEventsManagerTest, ShutdownClearsEverything)
{
    auto id = m_Registry.AddTrigger("TestEvent");
    m_Manager.PostTrigger(id);
    m_Manager.Shutdown();
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}
