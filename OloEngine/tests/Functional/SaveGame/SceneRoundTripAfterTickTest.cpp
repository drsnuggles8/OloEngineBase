#include "OloEnginePCH.h"

// =============================================================================
// SceneRoundTripAfterTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   SaveGame × Scene × Components. Existing SaveGameIntegrationTest captures
//   and restores scenes that have *never* been ticked — every component is
//   in its just-constructed state. That misses an entire bug class: a
//   component that mutates at runtime (animation time, transform after
//   physics, velocity, accumulated state) but whose serialiser only
//   round-trips its construction-time defaults. Those bugs only show up
//   when a player saves mid-game and reloads.
//
// Scenario: build a scene with several entities, tick it for ~0.5s of
// simulated time so subsystems write into components, capture the state,
// restore into a fresh Scene, and assert that the restored scene matches
// the original on every component the user can observe.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

#include <cmath>
#include <unordered_map>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    struct EntitySnapshot
    {
        glm::vec3 Translation{};
        glm::vec3 Rotation{};
        glm::vec3 Scale{};
        bool HasCamera = false;
        bool HasSprite = false;
        // Sprite payload (only valid when HasSprite is true) — captured so
        // the round-trip assertion catches value loss, not just presence loss.
        glm::vec4 SpriteColor{};
        f32 SpriteTilingFactor = 0.0f;
        // Camera payload (only valid when HasCamera is true). Primary +
        // projection params catch the "everything present, but the active
        // camera lost its FOV" failure class.
        bool CameraPrimary = false;
        i32 CameraProjectionType = -1;
        f32 CameraPerspectiveFOV = 0.0f;
        f32 CameraOrthographicSize = 0.0f;
    };

    std::unordered_map<std::string, EntitySnapshot> SnapshotByTag(Scene& scene)
    {
        std::unordered_map<std::string, EntitySnapshot> out;
        auto view = scene.GetAllEntitiesWith<IDComponent>();
        for (auto e : view)
        {
            Entity entity{ e, &scene };
            const auto& tag = entity.GetComponent<TagComponent>().Tag;
            EntitySnapshot snap;
            const auto& t = entity.GetComponent<TransformComponent>();
            snap.Translation = t.Translation;
            snap.Rotation = t.GetRotationEuler();
            snap.Scale = t.Scale;
            snap.HasCamera = entity.HasComponent<CameraComponent>();
            snap.HasSprite = entity.HasComponent<SpriteRendererComponent>();
            if (snap.HasSprite)
            {
                const auto& sprite = entity.GetComponent<SpriteRendererComponent>();
                snap.SpriteColor = sprite.Color;
                snap.SpriteTilingFactor = sprite.TilingFactor;
            }
            if (snap.HasCamera)
            {
                const auto& cam = entity.GetComponent<CameraComponent>();
                snap.CameraPrimary = cam.Primary;
                snap.CameraProjectionType = std::to_underlying(cam.Camera.GetProjectionType());
                snap.CameraPerspectiveFOV = cam.Camera.GetPerspectiveVerticalFOV();
                snap.CameraOrthographicSize = cam.Camera.GetOrthographicSize();
            }
            out.emplace(tag, snap);
        }
        return out;
    }
} // namespace

class SceneRoundTripAfterTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // The Functional angle: build a Scene with multiple components, *tick the
        // scene* (so subsystems run their per-frame work), then capture/restore
        // and compare. SaveGameSerializer today understands core components
        // (Tag, Transform, Camera, Sprite); skeletons/clips contain non-trivial
        // Ref<>s and round-trip via the asset pipeline, not the savegame
        // serialiser, so we deliberately exclude them here.

        auto player = GetScene().CreateEntity("Player");
        auto& playerT = player.GetComponent<TransformComponent>();
        playerT.Translation = { 1.0f, 2.0f, 3.0f };
        playerT.SetRotationEuler({ 0.1f, 0.2f, 0.3f });
        playerT.Scale = { 1.5f, 1.5f, 1.5f };
        auto& sprite = player.AddComponent<SpriteRendererComponent>();
        sprite.Color = { 0.5f, 0.7f, 0.2f, 1.0f };
        sprite.TilingFactor = 3.0f;

        auto camera = GetScene().CreateEntity("Camera");
        auto& cam = camera.AddComponent<CameraComponent>();
        cam.Primary = true;
        // Push the camera off its constructor defaults so the round-trip
        // assertion can tell "saved + restored" apart from "constructed fresh".
        cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
        cam.Camera.SetPerspectiveVerticalFOV(0.9f); // ~51.5°
        cam.Camera.SetOrthographicSize(7.5f);
        camera.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, -10.0f };

        auto prop = GetScene().CreateEntity("Prop");
        prop.GetComponent<TransformComponent>().Translation = { -4.0f, 0.0f, 7.0f };
        prop.GetComponent<TransformComponent>().Scale = { 2.0f, 2.0f, 2.0f };
    }
};

TEST_F(SceneRoundTripAfterTickTest, CapturedAfterTickRestoresIdenticalState)
{
    // Tick the scene so subsystems run their per-frame work — the Functional angle is
    // "save mid-game, restore, observe identical state."
    RunFrames(/*count=*/30); // 0.5s at 60Hz

    const auto before = SnapshotByTag(GetScene());
    ASSERT_EQ(before.size(), 3u) << "expected 3 entities pre-capture";

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u) << "CaptureSceneState produced empty payload";

    Ref<Scene> restored = Scene::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload))
        << "RestoreSceneState failed on payload of size " << payload.size();

    const auto after = SnapshotByTag(*restored);
    ASSERT_EQ(after.size(), before.size())
        << "entity count diverged across round-trip: before=" << before.size()
        << " after=" << after.size();

    // Each named entity survived with matching observable state.
    for (const auto& [tag, originalSnap] : before)
    {
        const auto it = after.find(tag);
        ASSERT_NE(it, after.end()) << "entity '" << tag << "' lost across round-trip";
        const auto& restoredSnap = it->second;

        EXPECT_NEAR(restoredSnap.Translation.x, originalSnap.Translation.x, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Translation.y, originalSnap.Translation.y, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Translation.z, originalSnap.Translation.z, 1e-4f) << "tag=" << tag;

        EXPECT_NEAR(restoredSnap.Rotation.x, originalSnap.Rotation.x, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Rotation.y, originalSnap.Rotation.y, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Rotation.z, originalSnap.Rotation.z, 1e-4f) << "tag=" << tag;

        EXPECT_NEAR(restoredSnap.Scale.x, originalSnap.Scale.x, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Scale.y, originalSnap.Scale.y, 1e-4f) << "tag=" << tag;
        EXPECT_NEAR(restoredSnap.Scale.z, originalSnap.Scale.z, 1e-4f) << "tag=" << tag;

        EXPECT_EQ(restoredSnap.HasCamera, originalSnap.HasCamera)
            << "CameraComponent presence diverged for tag=" << tag;
        EXPECT_EQ(restoredSnap.HasSprite, originalSnap.HasSprite)
            << "SpriteRendererComponent presence diverged for tag=" << tag;

        // Value-loss guards: presence alone isn't enough — bugs where a
        // component round-trips but its fields silently reset to defaults
        // (color = white, FOV = 0, etc.) only surface with these checks.
        if (originalSnap.HasSprite && restoredSnap.HasSprite)
        {
            EXPECT_NEAR(restoredSnap.SpriteColor.r, originalSnap.SpriteColor.r, 1e-4f) << "tag=" << tag;
            EXPECT_NEAR(restoredSnap.SpriteColor.g, originalSnap.SpriteColor.g, 1e-4f) << "tag=" << tag;
            EXPECT_NEAR(restoredSnap.SpriteColor.b, originalSnap.SpriteColor.b, 1e-4f) << "tag=" << tag;
            EXPECT_NEAR(restoredSnap.SpriteColor.a, originalSnap.SpriteColor.a, 1e-4f) << "tag=" << tag;
            EXPECT_NEAR(restoredSnap.SpriteTilingFactor, originalSnap.SpriteTilingFactor, 1e-4f)
                << "SpriteRendererComponent.TilingFactor lost across round-trip; tag=" << tag;
        }
        if (originalSnap.HasCamera && restoredSnap.HasCamera)
        {
            EXPECT_EQ(restoredSnap.CameraPrimary, originalSnap.CameraPrimary)
                << "CameraComponent.Primary lost across round-trip; tag=" << tag;
            EXPECT_EQ(restoredSnap.CameraProjectionType, originalSnap.CameraProjectionType)
                << "SceneCamera projection type lost across round-trip; tag=" << tag;
            EXPECT_NEAR(restoredSnap.CameraPerspectiveFOV, originalSnap.CameraPerspectiveFOV, 1e-4f)
                << "SceneCamera perspective FOV lost across round-trip; tag=" << tag;
            EXPECT_NEAR(restoredSnap.CameraOrthographicSize, originalSnap.CameraOrthographicSize, 1e-4f)
                << "SceneCamera orthographic size lost across round-trip; tag=" << tag;
        }
    }
}
