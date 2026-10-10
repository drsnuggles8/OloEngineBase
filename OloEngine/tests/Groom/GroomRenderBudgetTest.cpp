#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/Archive.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <limits>
#include <vector>

using namespace OloEngine;

TEST(GroomRenderBudget, SceneYamlPreservesTheAuthoredBudgetAndDefaultsMissingFields)
{
    auto scene = Scene::Create();
    Entity coat = scene->CreateEntity("Coat");
    auto& authored = coat.AddComponent<GroomComponent>();
    authored.m_MaxRenderStrands = 900000u;
    authored.m_MaxRenderSegments = 6000000u;
    const std::string yaml = SceneSerializer(scene).SerializeToYAML();

    auto loaded = Scene::Create();
    ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(yaml));
    const Entity reloaded = loaded->FindEntityByName("Coat");
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded.GetComponent<GroomComponent>(), authored);

    YAML::Node oldScene = YAML::Load(yaml);
    ASSERT_TRUE(oldScene["Entities"][0]["GroomComponent"].remove("MaxRenderSegments"));
    auto legacy = Scene::Create();
    ASSERT_TRUE(SceneSerializer(legacy).DeserializeFromYAML(YAML::Dump(oldScene)));
    const Entity oldCoat = legacy->FindEntityByName("Coat");
    ASSERT_TRUE(oldCoat);
    EXPECT_EQ(oldCoat.GetComponent<GroomComponent>().m_MaxRenderSegments, 2000000u);
}

TEST(GroomRenderBudget, SaveGamePreservesAndBoundsTheSegmentBudget)
{
    for (const auto [written, expected] : { std::pair{ 6000000u, 6000000u },
                                            std::pair{ 0u, 1u },
                                            std::pair{ std::numeric_limits<u32>::max(), 8000000u } })
    {
        GroomComponent authored;
        authored.m_MaxRenderStrands = 900000u;
        authored.m_MaxRenderSegments = written;
        authored.m_WidthScale = 1.25f;
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        writer.ArIsSaveGame = true;
        writer.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(writer, authored);

        GroomComponent loaded;
        FMemoryReader reader(bytes);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        ASSERT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd());
        EXPECT_EQ(loaded.m_MaxRenderStrands, 900000u);
        EXPECT_EQ(loaded.m_MaxRenderSegments, expected);
        EXPECT_FLOAT_EQ(loaded.m_WidthScale, 1.25f);
    }
}
