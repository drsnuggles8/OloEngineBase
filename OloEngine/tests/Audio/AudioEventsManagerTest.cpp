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
    // Add a Stop action — Stop actions don't create active sources (no audio file),
    // so we verify StopAll doesn't crash on an empty active set after processing.
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(id, action);

    m_Manager.PostTrigger(id);
    m_Manager.Update(OloEngine::Timestep(0.016f));
    // StopAll should complete without error even with zero active events
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
    // Process the pending event before shutdown so we test cleanup of processed state
    m_Manager.Update(OloEngine::Timestep(0.016f));
    m_Manager.Shutdown();
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
    // Re-init for TearDown's Shutdown call to be safe
    m_Manager.Init(&m_Registry);
}

// NOTE: Play actions with real audio files cannot be unit-tested here because
// AudioSource::Create requires an actual file on disk and the test environment
// does not provide one. The Update() → m_ActiveEvents population path is only
// exercised when std::filesystem::exists(path) returns true and
// Ref<AudioSource>::Create succeeds. A full integration test with a test .wav
// or a mock AudioSource factory would be needed to verify the Play→StopAll
// cleanup codepath end-to-end. The existing StopAllClearsActiveEvents test
// validates the StopAll logic itself (on an empty active set).
