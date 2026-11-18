#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <filesystem>

using namespace OloEngine;

class SaveGameIntegrationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = Ref<Scene>::Create();
        m_TempPath = OloEngine::Tests::TempFile("save.olosave");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    static u32 CountEntities(const Ref<Scene>& scene)
    {
        u32 count = 0;
        auto view = scene->GetAllEntitiesWith<IDComponent>();
        for ([[maybe_unused]] auto e : view)
        {
            ++count;
        }
        return count;
    }

    Ref<Scene> m_Scene;
    std::filesystem::path m_TempPath;
};

TEST_F(SaveGameIntegrationTest, CaptureAndRestoreEmptyScene)
{
    auto data = SaveGameSerializer::CaptureSceneState(*m_Scene);
    EXPECT_GT(data.size(), 0u);

    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, data));
}

TEST_F(SaveGameIntegrationTest, ContinueRetainsTheLoadedGeometryForMatchingAuthoredEntities)
{
    auto tower = m_Scene->CreateEntity("Tower");
    const UUID towerID = tower.GetUUID();
    const auto model = Ref<Model>::Create();
    auto& savedModel = tower.AddComponent<ModelComponent>(model, "Assets/Models/Tower.gltf");
    savedModel.m_Visible = false;
    auto bulb = m_Scene->CreateEntity("Optic");
    const UUID bulbID = bulb.GetUUID();
    const auto sphere = Ref<MeshSource>::Create();
    auto& savedMesh = bulb.AddComponent<MeshComponent>(sphere);
    savedMesh.m_Primitive = MeshPrimitive::Sphere;

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    savedModel.m_Visible = true;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto restoredTower = m_Scene->GetEntityByUUID(towerID);
    EXPECT_EQ(restoredTower.GetComponent<ModelComponent>().m_Model, model);
    EXPECT_FALSE(restoredTower.GetComponent<ModelComponent>().m_Visible);
    EXPECT_EQ(m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_MeshSource, sphere);

    // A different asset can occupy the same authored UUID in a later scene.
    // Its loaded geometry must not be attached to the saved identity.
    m_Scene->GetEntityByUUID(towerID).GetComponent<ModelComponent>().m_FilePath = "Assets/Models/Other.gltf";
    m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_Primitive = MeshPrimitive::Cube;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_NE(m_Scene->GetEntityByUUID(towerID).GetComponent<ModelComponent>().m_Model, model);
    EXPECT_NE(m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_MeshSource, sphere);
}

TEST_F(SaveGameIntegrationTest, ContinuePreservesTheAuthoredMeshSourceHandle)
{
    // The scene loader sets the resource handle when resolving MeshSourceHandle.
    // This test needs the identity and reference, without allocating a GPU mesh.
    struct BoundMeshSource final : MeshSource
    {
        explicit BoundMeshSource(AssetHandle handle)
        {
            SetHandle(handle);
        }
    };
    auto hull = m_Scene->CreateEntity("Boat Hull");
    const UUID hullID = hull.GetUUID();
    const Ref<MeshSource> authored = Ref<BoundMeshSource>::Create(AssetHandle(1234));
    hull.AddComponent<MeshComponent>(authored).m_LightmapStatic = true;
    auto transient = m_Scene->CreateEntity("Unregistered scratch mesh");
    const UUID transientID = transient.GetUUID();
    transient.AddComponent<MeshComponent>(Ref<MeshSource>::Create());

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    hull.GetComponent<MeshComponent>().m_LightmapStatic = false;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto& restored = m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>();
    EXPECT_EQ(restored.m_MeshSource, authored);
    EXPECT_TRUE(restored.m_LightmapStatic);
    EXPECT_FALSE(m_Scene->GetEntityByUUID(transientID).GetComponent<MeshComponent>().m_MeshSource);

    m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>().m_Primitive = MeshPrimitive::Cube;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto& differentBinding = m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>();
    EXPECT_EQ(differentBinding.m_Primitive, MeshPrimitive::None);
    EXPECT_NE(differentBinding.m_MeshSource, authored);
}

TEST_F(SaveGameIntegrationTest, CaptureAndRestoreWithEntities)
{
    // Create entities with various components
    Entity e1 = m_Scene->CreateEntity("Player");
    auto& transform = e1.GetComponent<TransformComponent>();
    transform.Translation = { 10.0f, 20.0f, 30.0f };
    transform.SetRotationEuler({ 0.1f, 0.2f, 0.3f });
    transform.Scale = { 2.0f, 2.0f, 2.0f };

    Entity e2 = m_Scene->CreateEntity("Camera");
    auto& cam = e2.AddComponent<CameraComponent>();
    cam.Primary = true;

    Entity e3 = m_Scene->CreateEntity("Sprite");
    auto& sprite = e3.AddComponent<SpriteRendererComponent>();
    sprite.Color = { 1.0f, 0.0f, 0.0f, 1.0f };
    sprite.TilingFactor = 2.5f;

    // Capture
    auto data = SaveGameSerializer::CaptureSceneState(*m_Scene);
    EXPECT_GT(data.size(), 0u);

    // Restore into fresh scene
    Ref<Scene> newScene = Ref<Scene>::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, data));

    // Verify entity count
    EXPECT_EQ(CountEntities(newScene), 3u);

    // Verify component data by finding entities by tag
    bool foundPlayer = false;
    bool foundCamera = false;
    bool foundSprite = false;
    auto view = newScene->GetAllEntitiesWith<IDComponent>();
    for (auto e : view)
    {
        Entity entity = { e, newScene.get() };
        const auto& tag = entity.GetComponent<TagComponent>().Tag;

        if (tag == "Player")
        {
            foundPlayer = true;
            const auto& t = entity.GetComponent<TransformComponent>();
            EXPECT_NEAR(t.Translation.x, 10.0f, 0.001f);
            EXPECT_NEAR(t.Translation.y, 20.0f, 0.001f);
            EXPECT_NEAR(t.Translation.z, 30.0f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().x, 0.1f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().y, 0.2f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().z, 0.3f, 0.001f);
            EXPECT_NEAR(t.Scale.x, 2.0f, 0.001f);
            EXPECT_NEAR(t.Scale.y, 2.0f, 0.001f);
            EXPECT_NEAR(t.Scale.z, 2.0f, 0.001f);
        }
        else if (tag == "Camera")
        {
            foundCamera = true;
            ASSERT_TRUE(entity.HasComponent<CameraComponent>());
            const auto& c = entity.GetComponent<CameraComponent>();
            EXPECT_TRUE(c.Primary);
        }
        else if (tag == "Sprite")
        {
            foundSprite = true;
            ASSERT_TRUE(entity.HasComponent<SpriteRendererComponent>());
            const auto& s = entity.GetComponent<SpriteRendererComponent>();
            EXPECT_NEAR(s.Color.r, 1.0f, 0.001f);
            EXPECT_NEAR(s.Color.g, 0.0f, 0.001f);
            EXPECT_NEAR(s.Color.b, 0.0f, 0.001f);
            EXPECT_NEAR(s.Color.a, 1.0f, 0.001f);
            EXPECT_NEAR(s.TilingFactor, 2.5f, 0.001f);
        }
        else
        {
            // No additional handling required.
        }
    }
    EXPECT_TRUE(foundPlayer);
    EXPECT_TRUE(foundCamera);
    EXPECT_TRUE(foundSprite);
}

// Skin diffusion became a scene setting in format 50 (issue #1689): a save
// carries it, and a restore replaces whatever the target scene held.
TEST_F(SaveGameIntegrationTest, SkinDiffusionSettingsSurviveCaptureAndRestore)
{
    SkinDiffusionSettings authored;
    authored.Enabled = false;
    authored.Quality = SkinDiffusionQuality::High;
    authored.DepthRejectionScale = 3.5f;
    authored.RadiusScale = 1.75f;
    ASSERT_NE(authored, SkinDiffusionSettings{});
    m_Scene->SetSkinDiffusionSettings(authored);

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Ref<Scene>::Create();
    restored->GetSkinDiffusionSettings().RadiusScale = 0.25f;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));
    EXPECT_EQ(restored->GetSkinDiffusionSettings(), authored);

    // The play-mode copy carries it too (scene-copy-must-carry-scene-level-settings.md).
    EXPECT_EQ(Scene::Copy(m_Scene)->GetSkinDiffusionSettings(), authored);
}

TEST_F(SaveGameIntegrationTest, FullFileRoundTrip)
{
    // Create test entity
    Entity e = m_Scene->CreateEntity("TestEntity");
    auto& t = e.GetComponent<TransformComponent>();
    t.Translation = { 1.0f, 2.0f, 3.0f };

    // Capture scene state
    auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ASSERT_GT(payload.size(), 0u);

    // Compress
    std::vector<u8> compressed;
    ASSERT_TRUE(SaveGameFile::Compress(payload, compressed));

    // Build metadata
    SaveGameMetadata meta;
    meta.DisplayName = "Integration Test";
    meta.SceneName = "TestScene";
    meta.TimestampUTC = 1700000000;
    meta.EntityCount = 1;

    // Build header
    SaveGameHeader header;
    header.EntityCount = 1;
    header.SetCompression(SaveGameCompression::Zlib);
    header.PayloadUncompressedSize = payload.size();

    // Write file
    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, {}, compressed));

    // Validate checksum
    EXPECT_TRUE(SaveGameFile::ValidateChecksum(m_TempPath));

    // Read metadata
    SaveGameHeader readHeader;
    SaveGameMetadata readMeta;
    ASSERT_TRUE(SaveGameFile::ReadMetadata(m_TempPath, readHeader, readMeta));
    EXPECT_EQ(readMeta.DisplayName, "Integration Test");
    EXPECT_EQ(readMeta.EntityCount, 1u);

    // Read and decompress payload
    std::vector<u8> readPayload;
    ASSERT_TRUE(SaveGameFile::ReadPayload(m_TempPath, readPayload));
    EXPECT_EQ(readPayload.size(), payload.size());
    EXPECT_EQ(readPayload, payload);

    // Restore into fresh scene
    Ref<Scene> newScene = Ref<Scene>::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, readPayload));

    // Verify
    EXPECT_EQ(CountEntities(newScene), 1u);
}

TEST_F(SaveGameIntegrationTest, EmptyPayloadReturnsError)
{
    std::vector<u8> empty;
    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*newScene, empty));
}

TEST_F(SaveGameIntegrationTest, CorruptedDataReturnsError)
{
    std::vector<u8> garbage = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };
    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*newScene, garbage));
}
