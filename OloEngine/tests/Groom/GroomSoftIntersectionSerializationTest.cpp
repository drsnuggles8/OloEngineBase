// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    TEST(GroomSoftIntersectionSerialization, SceneYamlKeepsAllOverrides)
    {
        auto scene = Scene::Create();
        auto entity = scene->CreateEntity("Contact coat");
        auto& component = entity.AddComponent<GroomSoftIntersectionComponent>();
        component.m_FadeDistance = 0.004f;
        component.m_ProtectedRoot = 0.4f;
        component.m_Automatic = false;
        component.m_Enabled = false;
        const auto expected = component;
        const auto yaml = SceneSerializer(scene).SerializeToYAML();
        auto loaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(yaml));
        const auto coat = loaded->FindEntityByName("Contact coat");
        ASSERT_TRUE(coat && coat.HasComponent<GroomSoftIntersectionComponent>());
        EXPECT_TRUE(coat.GetComponent<GroomSoftIntersectionComponent>() == expected);
    }

    TEST(GroomSoftIntersectionSerialization, MissingFieldsKeepAutomaticProtectedDefaults)
    {
        auto loaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(
            "Scene: Contact\nVersion: 1\nEntities:\n  - Entity: 1614\n    TagComponent:\n      Tag: Coat\n    GroomSoftIntersectionComponent: {}\n"));
        const auto coat = loaded->FindEntityByName("Coat");
        ASSERT_TRUE(coat && coat.HasComponent<GroomSoftIntersectionComponent>());
        EXPECT_TRUE(coat.GetComponent<GroomSoftIntersectionComponent>() == GroomSoftIntersectionComponent{});
    }

    TEST(GroomSoftIntersectionSerialization, SaveGamePayloadRoundTripsAndConsumesExactlyItsFields)
    {
        GroomSoftIntersectionComponent authored;
        authored.m_FadeDistance = 0.006f;
        authored.m_ProtectedRoot = 0.15f;
        authored.m_Automatic = false;
        authored.m_Enabled = false;
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        SaveGameComponentSerializer::Serialize(writer, authored);
        GroomSoftIntersectionComponent loaded;
        FMemoryReader reader(bytes);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd());
        EXPECT_TRUE(loaded == authored);
    }

    TEST(GroomSoftIntersectionSerialization, SaveGameSanitizesCorruptFloats)
    {
        GroomSoftIntersectionComponent authored;
        authored.m_FadeDistance = std::numeric_limits<f32>::quiet_NaN();
        authored.m_ProtectedRoot = std::numeric_limits<f32>::infinity();
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        SaveGameComponentSerializer::Serialize(writer, authored);
        GroomSoftIntersectionComponent loaded;
        FMemoryReader reader(bytes);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FLOAT_EQ(loaded.m_FadeDistance, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_ProtectedRoot, 0.25f);
    }

    TEST(GroomSoftIntersectionSerialization, BinarySidecarAndCookedSceneKeepOverrides)
    {
        auto authored = Scene::Create();
        auto entity = authored->CreateEntity("Packed coat");
        auto& component = entity.AddComponent<GroomSoftIntersectionComponent>();
        component.m_FadeDistance = 0.012f;
        component.m_ProtectedRoot = 0.6f;
        component.m_Automatic = false;
        const auto expected = component;
        const auto path = TempDir() / "contact.olo";
        SceneSerializer(authored).Serialize(path);
        auto first = Scene::Create();
        ASSERT_TRUE(SceneSerializer(first).Deserialize(path));
        ASSERT_TRUE(std::filesystem::exists(path.string() + ".scenebin"));
        auto cached = Scene::Create();
        ASSERT_TRUE(SceneSerializer(cached).Deserialize(path));
        EXPECT_TRUE(cached->FindEntityByName("Packed coat").GetComponent<GroomSoftIntersectionComponent>() == expected);

        // The cooked scene decoder consumes the same YAML component stream
        // as the production writer. No asset references or graphics context
        // are substituted for this component-only archive contract.
        SceneAssetSerializer serializer;
        const auto yaml = serializer.SerializeToString(authored);
        const auto pack = TempDir() / "contact.olopack";
        AssetPackFile::SceneInfo info{};
        info.Handle = 1614;
        {
            FileStreamWriter writer(pack);
            ASSERT_TRUE(writer.IsStreamGood());
            writer.WriteRaw(static_cast<u32>(yaml.size()));
            writer.WriteData(yaml.data(), yaml.size());
            info.PackedSize = writer.GetStreamPosition();
        }
        FileStreamReader reader(pack);
        auto loaded = serializer.DeserializeSceneFromAssetPack(reader, info);
        ASSERT_TRUE(loaded);
        EXPECT_TRUE(loaded->FindEntityByName("Packed coat").GetComponent<GroomSoftIntersectionComponent>() == expected);
    }
} // namespace OloEngine::Tests
