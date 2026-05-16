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
// AudioListener has no GetPosition (writes go straight to miniaudio's
// internal listener slot), so we can't directly read the synced position.
// The strong signal here is the seam wiring: InitAudioRuntime allocates
// the Ref<>, OnUpdateRuntime's loop runs without dereferencing a null Ref<>.
// A regression that drops the listener iteration or stops invoking
// InitAudioRuntime crashes the harness here.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Audio/AudioListener.h"

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
