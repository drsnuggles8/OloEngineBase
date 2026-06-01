#include "OloEnginePCH.h"

// =============================================================================
// CinematicAssetPlaybackTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Asset pipeline × Cinematic × Scene::OnUpdateRuntime. This is the exact
//   mechanism the editor demo (CinematicDemo.olo + IntroSequence.olocine)
//   relies on: a `.olocine` is imported through the EditorAssetManager, the
//   CinematicComponent references it by handle (NOT a pre-set RuntimeSequence),
//   and CinematicSystem resolves handle → asset → playback while ticking the
//   real Scene. It double-checks the shipped demo asset parses and drives the
//   demo's entity UUIDs, so the editor walkthrough can't silently rot.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // These MUST match the UUIDs the shipped IntroSequence.olocine targets and
    // the entities in CinematicDemo.olo.
    constexpr u64 kCameraUUID = 8801000000000001ULL;
    constexpr u64 kMovingSpriteUUID = 8801000000000002ULL;
    constexpr u64 kDirectorUUID = 8801000000000004ULL;
} // namespace

class CinematicAssetPlaybackTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Stage + import the real shipped sequence asset through the editor
        // asset pipeline (extension map → CinematicSequenceAssetSerializer).
        EnableAssetManager({ "Assets/Cinematics/IntroSequence.olocine" });
        auto manager = Project::GetAssetManager().As<EditorAssetManager>();
        ASSERT_TRUE(manager) << "EditorAssetManager not active after EnableAssetManager";
        m_SequenceHandle = manager->ImportAsset(StagedAssetAbsolutePath("Assets/Cinematics/IntroSequence.olocine"));
        ASSERT_NE(m_SequenceHandle, 0u) << "Importing IntroSequence.olocine returned a zero handle";

        // Rebuild the demo's targeted entities with the SAME fixed UUIDs.
        m_Camera = GetScene().CreateEntityWithUUID(OloEngine::UUID(kCameraUUID), "MainCamera");
        m_Camera.AddComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(0.6f);
        m_Camera.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 6.0f };

        m_Sprite = GetScene().CreateEntityWithUUID(OloEngine::UUID(kMovingSpriteUUID), "MovingSprite");
        m_Sprite.GetComponent<TransformComponent>().Translation = { -2.0f, 0.0f, 0.0f };

        // Director references the sequence by HANDLE only — the system must
        // resolve it through the asset manager (RuntimeSequence left null).
        m_Director = GetScene().CreateEntityWithUUID(OloEngine::UUID(kDirectorUUID), "Director");
        auto& cine = m_Director.AddComponent<CinematicComponent>();
        cine.Sequence = m_SequenceHandle;
        cine.PlayFromStart();
    }

    AssetHandle m_SequenceHandle = 0;
    Entity m_Camera, m_Sprite, m_Director;
};

TEST_F(CinematicAssetPlaybackTest, ImportedSequenceDrivesEntitiesByHandle)
{
    // The asset parsed into the expected shape.
    auto seq = AssetManager::GetAsset<CinematicSequence>(m_SequenceHandle);
    ASSERT_TRUE(seq) << "GetAsset<CinematicSequence> returned null for the imported handle";
    EXPECT_NEAR(seq->GetEffectiveDuration(), 8.0f, 1e-3f);
    ASSERT_EQ(seq->CameraTracks.size(), 1u);
    ASSERT_EQ(seq->TransformTracks.size(), 1u);
    EXPECT_EQ(static_cast<u64>(seq->CameraTracks[0].Target), kCameraUUID);
    EXPECT_EQ(static_cast<u64>(seq->TransformTracks[0].Target), kMovingSpriteUUID);

    // Confirm the system lazily resolved the handle into RuntimeSequence on the
    // first tick (proves the handle path, not a pre-set Ref).
    RunFrames(1);
    EXPECT_TRUE(m_Director.GetComponent<CinematicComponent>().RuntimeSequence)
        << "CinematicSystem did not resolve the sequence from its asset handle";

    // Drive to ~3.9s (just before the t=4 midpoint), still mid-timeline.
    TickFor(3.9f);

    // Moving sprite eased from x=-2 toward x=+2.
    const f32 spriteX = m_Sprite.GetComponent<TransformComponent>().Translation.x;
    EXPECT_GT(spriteX, 1.0f) << "moving sprite did not advance along its translation track";

    // Camera panned left (x: 0 -> -3) and zoomed (FOV 0.6 -> 0.95).
    const f32 camX = m_Camera.GetComponent<TransformComponent>().Translation.x;
    EXPECT_LT(camX, -1.0f) << "camera track did not pan the camera entity";
    const f32 fov = m_Camera.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV();
    EXPECT_GT(fov, 0.7f) << "camera track did not drive the perspective FOV";

    const auto& cine = m_Director.GetComponent<CinematicComponent>();
    EXPECT_TRUE(cine.Playing);
    EXPECT_FALSE(cine.Finished);
}
