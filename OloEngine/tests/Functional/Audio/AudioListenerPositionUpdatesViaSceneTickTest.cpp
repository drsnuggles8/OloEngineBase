#include "OloEnginePCH.h"

// =============================================================================
// AudioRuntimeTicksViaSceneTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × Scene::m_AudioEventsManager × AudioListenerComponent.
//   Scene::OnUpdateRuntime contains:
//     (a) `m_AudioEventsManager->Update(ts)` — drains queued audio events
//     (b) a loop over Active AudioListenerComponents that resyncs
//         listener.SetPosition/SetDirection from TransformComponent.
//   Both rely on (1) InitAudioRuntime having created m_AudioEventsManager
//   and the listener Ref<AudioListener>, and (2) OnUpdateRuntime actually
//   invoking the audio block (not gated on a state we forgot to set).
//
// Scenario: an Active AudioListenerComponent on an entity. Run InitAudioRuntime,
// tick several frames. Assert the listener Ref<> persists and the scene
// stays alive — the audio loop must run without crashing on the listener's
// SetPosition call.
//
// VoiceManager exposes the exact position used for voice admission, even
// when the test harness has no miniaudio device.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/VoiceManager.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AudioRuntimeTicksViaSceneTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Listener = GetScene().CreateEntityWithUUID(UUID{ 777 }, "Listener");
        m_Listener.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
        auto& ac = m_Listener.AddComponent<AudioListenerComponent>();
        ac.Active = true;

        EnableAudio();
    }

    Entity m_Listener;
};

TEST_F(AudioRuntimeTicksViaSceneTest, ParentedListenerScoresVoicesFromItsWorldPosition)
{
    Entity parent = GetScene().CreateEntity("Listener Rig");
    parent.GetComponent<TransformComponent>().Translation = { 20.0f, 4.0f, -7.0f };
    parent.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, glm::half_pi<f32>(), 0.0f });
    m_Listener.SetParent(parent);

    RunFrames(1);
    const glm::vec3 parented = Audio::VoiceManager::Get().GetListenerPosition();
    const glm::vec3 expected(23.0f, 6.0f, -8.0f);
    EXPECT_NEAR(glm::length(parented - expected), 0.0f, 1.0e-4f);

    Entity twin = GetScene().CreateEntity("Root Listener");
    twin.GetComponent<TransformComponent>().Translation = expected;
    auto& twinAudio = twin.AddComponent<AudioListenerComponent>();
    twinAudio.Active = true;
    m_Listener.GetComponent<AudioListenerComponent>().Active = false;
    RunFrames(1);
    EXPECT_TRUE(twinAudio.Listener);
    EXPECT_NEAR(glm::length(Audio::VoiceManager::Get().GetListenerPosition() - parented), 0.0f, 1.0e-4f);
}

TEST_F(AudioRuntimeTicksViaSceneTest, ParentedSoundGraphTracksTheRootTwin)
{
    using Audio::SoundGraph::SoundGraphSound;
    Entity parent = GetScene().CreateEntity("Sound Graph Rig");
    parent.GetComponent<TransformComponent>().Translation = { 20.0f, -4.0f, 0.0f };
    auto addSound = [this](glm::vec3 position)
    {
        Entity entity = GetScene().CreateEntity("Sound Graph");
        entity.GetComponent<TransformComponent>().Translation = position;
        auto& component = entity.AddComponent<AudioSoundGraphComponent>();
        component.Sound = Ref<SoundGraphSound>::Create();
        EXPECT_TRUE(component.Sound->InitializeDetachedSource());
        return entity;
    };
    Entity child = addSound({ 2.0f, 4.0f, 0.0f });
    Entity root = addSound({ 22.0f, 0.0f, 0.0f });
    child.SetParent(parent);
    auto childSound = child.GetComponent<AudioSoundGraphComponent>().Sound;
    auto rootSound = root.GetComponent<AudioSoundGraphComponent>().Sound;

    RunFrames(1);
    EXPECT_NEAR(glm::length(childSound->GetLocation() - glm::vec3(22.0f, 0.0f, 0.0f)), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::length(childSound->GetLocation() - rootSound->GetLocation()), 0.0f, 1.0e-4f);

    parent.GetComponent<TransformComponent>().Translation.x += 3.0f;
    root.GetComponent<TransformComponent>().Translation.x += 3.0f;
    RunFrames(1);
    EXPECT_NEAR(glm::length(childSound->GetLocation() - rootSound->GetLocation()), 0.0f, 1.0e-4f);
}

TEST_F(AudioRuntimeTicksViaSceneTest, DirectSourcePoseMatchesItsRootTwinBeforePropagation)
{
    Entity parent = GetScene().CreateEntity("Source Rig");
    parent.GetComponent<TransformComponent>().Translation = { 20.0f, 4.0f, -7.0f };
    parent.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, glm::half_pi<f32>(), 0.0f });
    Entity child = GetScene().CreateEntity("Child Source");
    child.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
    child.SetParent(parent);
    Entity root = GetScene().CreateEntity("Root Source");
    root.GetComponent<TransformComponent>().Translation = { 23.0f, 6.0f, -8.0f };
    root.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, glm::half_pi<f32>(), 0.0f });

    const auto childPose = GetScene().GetAudioSourceWorldPose(child);
    const auto rootPose = GetScene().GetAudioSourceWorldPose(root);
    EXPECT_NEAR(glm::length(childPose.Position - rootPose.Position), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::length(childPose.Orientation - rootPose.Orientation), 0.0f, 1.0e-4f);
    EXPECT_NEAR(glm::length(childPose.Up - rootPose.Up), 0.0f, 1.0e-4f);
}

TEST_F(AudioRuntimeTicksViaSceneTest, ListenerRefIsAllocatedAndTickingRunsCleanly)
{
    auto& ac = m_Listener.GetComponent<AudioListenerComponent>();
    ASSERT_TRUE(ac.Listener)
        << "Scene::InitAudioRuntime did not allocate Ref<AudioListener> for "
           "an entity with AudioListenerComponent — listener iteration is broken.";

    // Tick many frames with the listener active. If the audio loop in
    // OnUpdateRuntime dereferences a null Ref<> or hits stale state, this
    // crashes (the assertion only runs if we survive).
    RunFrames(30); // 0.5s

    EXPECT_TRUE(ac.Listener)
        << "Listener Ref<> went null during ticking";
    EXPECT_TRUE(ac.Active)
        << "Active flag was mutated by the sync loop";

    // Move the entity, tick more. The listener SetPosition path runs each
    // tick — crashing or asserting here would mean the listener writes
    // (which go into miniaudio internals) blew up on a stale handle.
    m_Listener.GetComponent<TransformComponent>().Translation = { -5.0f, 10.0f, 4.0f };
    RunFrames(10);

    EXPECT_TRUE(ac.Listener);
    EXPECT_TRUE(ac.Active);
}
