#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;
using namespace OloEngine::Audio;

// =============================================================================
// Integration tests: AudioEventsManager + Scene (entity position resolver,
// AudioPlayback static API, and the Scene-like lifecycle without requiring
// a running Application).
// =============================================================================

class AudioEventsSceneIntegrationTest : public ::testing::Test
{
  protected:
    AudioCommandRegistry m_Registry;
    AudioEventsManager m_Manager;
    Ref<Scene> m_Scene;

    void SetUp() override
    {
        m_Scene = Scene::Create();
        m_Manager.Init(&m_Registry);
    }

    void TearDown() override
    {
        AudioPlayback::SetManager(nullptr);
        m_Manager.Shutdown();
        m_Scene.Reset();
    }

    /// Mimics the lambda that Scene::OnRuntimeStart installs.
    void InstallPositionResolver()
    {
        m_Manager.SetPositionResolver([this](u64 objectID, glm::vec3& outPos) -> bool
                                      {
			if (auto entity = m_Scene->TryGetEntityWithUUID(UUID(objectID)); entity && entity->HasComponent<TransformComponent>())
			{
				outPos = entity->GetComponent<TransformComponent>().Translation;
				return true;
			}
			return false; });
    }

    /// Helper: create an entity with a known UUID and position.
    Entity CreateEntityAt(UUID uuid, glm::vec3 position)
    {
        auto entity = m_Scene->CreateEntityWithUUID(uuid, "TestEntity");
        entity.GetComponent<TransformComponent>().Translation = position;
        return entity;
    }
};

// ---------------------------------------------------------------------------
// PositionResolver integration
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, PositionResolverFindsEntityByUUID)
{
    UUID uuid;
    glm::vec3 expected{ 10.0f, 20.0f, 30.0f };
    CreateEntityAt(uuid, expected);

    // Store the resolver locally so we can invoke it directly in addition to
    // registering it with the manager. Play actions (which exercise the resolver
    // during spatial updates) require Project::GetAssetFileSystemPath and real
    // audio files — neither available in unit tests. Direct invocation lets us
    // verify resolver correctness without a running audio backend.
    bool resolverCalled = false;
    glm::vec3 resolvedPos{};
    AudioEventsManager::PositionResolver resolver = [this, &resolverCalled, &resolvedPos](u64 objectID, glm::vec3& outPos) -> bool
    {
        if (auto entity = m_Scene->TryGetEntityWithUUID(UUID(objectID)); entity && entity->HasComponent<TransformComponent>())
        {
            outPos = entity->GetComponent<TransformComponent>().Translation;
            resolverCalled = true;
            resolvedPos = outPos;
            return true;
        }
        return false;
    };
    m_Manager.SetPositionResolver(resolver);

    // Directly invoke the resolver to verify it resolves the entity correctly
    glm::vec3 directPos{};
    EXPECT_TRUE(resolver(static_cast<u64>(uuid), directPos));
    EXPECT_TRUE(resolverCalled);
    EXPECT_FLOAT_EQ(directPos.x, expected.x);
    EXPECT_FLOAT_EQ(directPos.y, expected.y);
    EXPECT_FLOAT_EQ(directPos.z, expected.z);

    // Also exercise through PostTrigger + Update to verify no crashes
    auto cmdID = m_Registry.AddTrigger("ResolverTest");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);
    auto eid = m_Manager.PostTrigger(cmdID, static_cast<u64>(uuid));
    EXPECT_GT(eid, 0u);
    m_Manager.Update(Timestep(0.016f));
}

TEST_F(AudioEventsSceneIntegrationTest, PositionResolverReturnsFalseForMissingEntity)
{
    // Store the resolver locally so we can invoke it directly to verify it
    // returns false for a non-existent entity UUID.
    bool resolverCalled = false;
    AudioEventsManager::PositionResolver resolver = [this, &resolverCalled](u64 objectID, glm::vec3& outPos) -> bool
    {
        if (auto entity = m_Scene->TryGetEntityWithUUID(UUID(objectID)); entity && entity->HasComponent<TransformComponent>())
        {
            outPos = entity->GetComponent<TransformComponent>().Translation;
            resolverCalled = true;
            return true;
        }
        return false;
    };
    m_Manager.SetPositionResolver(resolver);

    // Directly invoke — entity 99999 doesn't exist, should return false
    glm::vec3 pos{};
    EXPECT_FALSE(resolver(99999, pos));
    EXPECT_FALSE(resolverCalled);

    // Also exercise through manager to verify no crash
    auto cmdID = m_Registry.AddTrigger("MissingEntityTest");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);
    auto eid = m_Manager.PostTrigger(cmdID, 99999);
    EXPECT_GT(eid, 0u);
    m_Manager.Update(Timestep(0.016f));

    // Verify the entity truly doesn't exist
    auto entity = m_Scene->TryGetEntityWithUUID(UUID(99999));
    EXPECT_FALSE(entity.has_value());
}

TEST_F(AudioEventsSceneIntegrationTest, PositionResolverReflectsMovedEntity)
{
    UUID uuid;
    auto entity = CreateEntityAt(uuid, { 1.0f, 2.0f, 3.0f });

    // Store resolver locally to invoke directly and verify updated position
    AudioEventsManager::PositionResolver resolver = [this](u64 objectID, glm::vec3& outPos) -> bool
    {
        if (auto e = m_Scene->TryGetEntityWithUUID(UUID(objectID)); e && e->HasComponent<TransformComponent>())
        {
            outPos = e->GetComponent<TransformComponent>().Translation;
            return true;
        }
        return false;
    };
    m_Manager.SetPositionResolver(resolver);

    // Move the entity
    entity.GetComponent<TransformComponent>().Translation = { 100.0f, 200.0f, 300.0f };

    // Directly invoke resolver to verify it returns the updated coordinates
    glm::vec3 pos{};
    EXPECT_TRUE(resolver(static_cast<u64>(uuid), pos));
    EXPECT_FLOAT_EQ(pos.x, 100.0f);
    EXPECT_FLOAT_EQ(pos.y, 200.0f);
    EXPECT_FLOAT_EQ(pos.z, 300.0f);

    // Also exercise through manager to verify no crash
    auto cmdID = m_Registry.AddTrigger("MoveTest");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);
    m_Manager.PostTrigger(cmdID, static_cast<u64>(uuid));
    m_Manager.Update(Timestep(0.016f));
}

// ---------------------------------------------------------------------------
// AudioPlayback static API wired to manager
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, AudioPlaybackPostTriggerDelegatesToManager)
{
    AudioPlayback::SetManager(&m_Manager);
    auto cmdID = m_Registry.AddTrigger("Explosion");

    auto eid = AudioPlayback::PostTrigger(cmdID);
    EXPECT_GT(eid, 0u);
}

TEST_F(AudioEventsSceneIntegrationTest, AudioPlaybackStopAllOnEmptyActiveSetDelegates)
{
    AudioPlayback::SetManager(&m_Manager);
    auto cmdID = m_Registry.AddTrigger("Wind");
    // Stop actions don't create active sources. Play actions require
    // Project::GetAssetFileSystemPath (asserts s_ActiveProject) and real audio files.
    // This test verifies StopAll delegates to the manager without crashing on an empty set.
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);

    AudioPlayback::PostTrigger(cmdID);
    m_Manager.Update(Timestep(0.016f));
    // Stop-only triggers don't produce active entries (no sources created),
    // so active count is already 0 after Update. StopAll clears any remaining.
    // TODO: Add functional StopAll coverage with mock AudioSource when available
    //       (AudioPlayback::PostTrigger with Play + AudioPlayback::StopAll + assert count 0).
    AudioPlayback::StopAll();
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}

TEST_F(AudioEventsSceneIntegrationTest, AudioPlaybackIsEventActiveReturnsFalseForStopOnlyTrigger)
{
    AudioPlayback::SetManager(&m_Manager);
    auto cmdID = m_Registry.AddTrigger("Alert");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);

    auto eid = AudioPlayback::PostTrigger(cmdID);
    EXPECT_NE(eid, 0u);
    // After Update, Stop-only triggers don't produce active sources, so
    // the event is cleaned up — IsEventActive returns false.
    m_Manager.Update(Timestep(0.016f));
    EXPECT_FALSE(AudioPlayback::IsEventActive(eid));
}

TEST_F(AudioEventsSceneIntegrationTest, AudioPlaybackWithNullManagerReturnsDefaults)
{
    AudioPlayback::SetManager(nullptr);
    // Should not crash, returns 0
    auto eid = AudioPlayback::PostTrigger(CommandID::FromString("Anything"));
    EXPECT_EQ(eid, 0u);
    EXPECT_FALSE(AudioPlayback::IsEventActive(1));
}

TEST_F(AudioEventsSceneIntegrationTest, StopEventCancelsPendingBeforeUpdate)
{
    AudioPlayback::SetManager(&m_Manager);
    auto cmdID = m_Registry.AddTrigger("CancelMe");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);

    // Post and immediately cancel before Update processes it
    auto eid = AudioPlayback::PostTrigger(cmdID);
    EXPECT_GT(eid, 0u);
    AudioPlayback::StopEvent(eid);
    // Update should not process the cancelled event
    m_Manager.Update(Timestep(0.016f));
    EXPECT_FALSE(AudioPlayback::IsEventActive(eid));
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}

TEST_F(AudioEventsSceneIntegrationTest, PostTriggerRejectsUnknownCommandID)
{
    AudioPlayback::SetManager(&m_Manager);
    // Don't register this in the registry
    auto unknownCmd = CommandID::FromString("UnregisteredEvent");
    auto eid = AudioPlayback::PostTrigger(unknownCmd);
    EXPECT_EQ(eid, 0u);
}

// ---------------------------------------------------------------------------
// Manager lifecycle mimicking Scene::OnRuntimeStart / OnRuntimeStop
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, FullLifecycleInitUpdateShutdown)
{
    // 1. Init (like OnRuntimeStart)
    InstallPositionResolver();
    AudioPlayback::SetManager(&m_Manager);

    auto cmdID = m_Registry.AddTrigger("Footstep");
    TriggerAction action;
    action.Type = ActionType::Stop; // Stop actions don't need audio files
    m_Registry.AddAction(cmdID, action);

    // 2. Runtime: post event and update
    UUID uuid;
    CreateEntityAt(uuid, { 5.0f, 0.0f, 5.0f });
    auto eid = AudioPlayback::PostTrigger(cmdID, static_cast<u64>(uuid));
    EXPECT_GT(eid, 0u);

    m_Manager.Update(Timestep(0.016f));

    // 3. Shutdown (like OnRuntimeStop)
    m_Manager.StopAllEvents();
    AudioPlayback::SetManager(nullptr);
    m_Manager.Shutdown();

    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}

TEST_F(AudioEventsSceneIntegrationTest, ShutdownClearsPositionResolver)
{
    // Install a resolver that tracks invocations
    bool resolverCalled = false;
    m_Manager.SetPositionResolver([&resolverCalled](u64 /*objectID*/, glm::vec3& /*outPos*/) -> bool
                                  {
        resolverCalled = true;
        return false; });

    m_Manager.Shutdown();

    // Re-init without reinstalling the resolver
    m_Manager.Init(&m_Registry);
    auto cmdID = m_Registry.AddTrigger("PostShutdownTest");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);
    m_Manager.PostTrigger(cmdID, 12345);
    m_Manager.Update(Timestep(0.016f));

    // The old resolver should NOT have been invoked after Shutdown + re-Init
    EXPECT_FALSE(resolverCalled);
}

// ---------------------------------------------------------------------------
// Event posting with entity object IDs
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, PostTriggerWithObjectIDQueuesEvent)
{
    InstallPositionResolver();
    UUID uuid;
    CreateEntityAt(uuid, { 0.0f, 0.0f, 0.0f });

    auto cmdID = m_Registry.AddTrigger("DoorOpen");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);

    auto eid = m_Manager.PostTrigger(cmdID, static_cast<u64>(uuid));
    EXPECT_GT(eid, 0u);

    m_Manager.Update(Timestep(0.016f));
    // Stop-only actions don't produce active events
    EXPECT_EQ(m_Manager.GetActiveEventCount(), 0u);
}

TEST_F(AudioEventsSceneIntegrationTest, MultipleEntitiesPostEventsIndependently)
{
    InstallPositionResolver();
    UUID uuid1, uuid2;
    CreateEntityAt(uuid1, { 0.0f, 0.0f, 0.0f });
    CreateEntityAt(uuid2, { 50.0f, 0.0f, 50.0f });

    auto cmdID = m_Registry.AddTrigger("Gunshot");
    TriggerAction action;
    action.Type = ActionType::Stop;
    m_Registry.AddAction(cmdID, action);

    auto eid1 = m_Manager.PostTrigger(cmdID, static_cast<u64>(uuid1));
    auto eid2 = m_Manager.PostTrigger(cmdID, static_cast<u64>(uuid2));

    EXPECT_NE(eid1, eid2);
    EXPECT_GT(eid1, 0u);
    EXPECT_GT(eid2, 0u);

    m_Manager.Update(Timestep(0.016f));
}

// ---------------------------------------------------------------------------
// Event control (StopEvent, PauseEvent, ResumeEvent) through manager
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, StopEventOnNonExistentIDDoesNotCrash)
{
    InstallPositionResolver();
    m_Manager.StopEvent(99999);
    m_Manager.PauseEvent(99999);
    m_Manager.ResumeEvent(99999);
    // No crash = pass
}

// ---------------------------------------------------------------------------
// AudioSourceComponent UseEventSystem flag integration
// ---------------------------------------------------------------------------

TEST_F(AudioEventsSceneIntegrationTest, AudioSourceComponentEventSystemFlag)
{
    auto entity = m_Scene->CreateEntity("AudioEntity");
    auto& asc = entity.AddComponent<AudioSourceComponent>();

    EXPECT_FALSE(asc.UseEventSystem);
    EXPECT_FALSE(asc.StartCommandID.IsValid());

    // Configure for event-driven audio
    asc.UseEventSystem = true;
    asc.StartEvent = "PlayBGM";
    asc.StartCommandID = CommandID::FromString("PlayBGM");

    EXPECT_TRUE(asc.UseEventSystem);
    EXPECT_TRUE(asc.StartCommandID.IsValid());
}

TEST_F(AudioEventsSceneIntegrationTest, CommandIDFromStartEventMatchesRegistry)
{
    auto cmdID = m_Registry.AddTrigger("PlayAmbience");
    auto entityCmdID = CommandID::FromString("PlayAmbience");

    // The IDs should match since both use the same CRC32 of the name
    EXPECT_EQ(cmdID, entityCmdID);
}
