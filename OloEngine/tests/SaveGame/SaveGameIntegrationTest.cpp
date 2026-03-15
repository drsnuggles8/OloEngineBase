#include <gtest/gtest.h>

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
        m_TempPath = std::filesystem::temp_directory_path() / "olo_integration_test.olosave";
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
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

TEST_F(SaveGameIntegrationTest, CaptureAndRestoreWithEntities)
{
    // Create entities with various components
    Entity e1 = m_Scene->CreateEntity("Player");
    auto& transform = e1.GetComponent<TransformComponent>();
    transform.Translation = { 10.0f, 20.0f, 30.0f };
    transform.Rotation = { 0.1f, 0.2f, 0.3f };
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
    u32 entityCount = 0;
    auto view = newScene->GetAllEntitiesWith<IDComponent>();
    for ([[maybe_unused]] auto e : view)
    {
        ++entityCount;
    }
    EXPECT_EQ(entityCount, 3u);

    // Verify component data by finding entities by tag
    bool foundPlayer = false, foundCamera = false, foundSprite = false;
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
            EXPECT_NEAR(t.Rotation.x, 0.1f, 0.001f);
            EXPECT_NEAR(t.Rotation.y, 0.2f, 0.001f);
            EXPECT_NEAR(t.Rotation.z, 0.3f, 0.001f);
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
    }
    EXPECT_TRUE(foundPlayer);
    EXPECT_TRUE(foundCamera);
    EXPECT_TRUE(foundSprite);
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
    u32 count = 0;
    auto view = newScene->GetAllEntitiesWith<IDComponent>();
    for ([[maybe_unused]] auto ent : view)
    {
        ++count;
    }
    EXPECT_EQ(count, 1u);
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
