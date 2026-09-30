// OLO_TEST_LAYER: unit
// =============================================================================
// FoliageShadowCasterSaveLoadTest -- issue #1533.
//
// FoliageLayer::CastShadows keeps a layer out of the shadow maps (it still
// receives). It defaults on, which is what every layer did before it existed,
// so the load paths have one job each: carry an authored "off" across scene
// YAML and a save game, and leave every older file casting.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        Entity FindByTag(Scene& scene, const char* tag)
        {
            for (auto e : scene.GetAllEntitiesWith<TagComponent>())
            {
                Entity ent{ e, &scene };
                if (ent.GetComponent<TagComponent>().Tag == tag)
                    return ent;
            }
            return {};
        }

        [[nodiscard]] FoliageComponent MakeLawn(bool castShadows)
        {
            FoliageComponent foliage;
            FoliageLayer layer;
            layer.Name = "Lawn";
            layer.CastShadows = castShadows;
            foliage.m_Layers.Add(layer);
            return foliage;
        }
    } // namespace

    TEST(FoliageShadowCasterSaveLoad, SceneYamlRoundTripsALayerKeptOutOfTheShadowMaps)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity ground = scene->CreateEntity("Lawn");
            ground.AddComponent<TerrainComponent>();
            ground.AddComponent<FoliageComponent>(MakeLawn(false));
            yaml = SceneSerializer(scene).SerializeToYAML();
        }
        ASSERT_FALSE(yaml.empty());
        EXPECT_NE(yaml.find("CastShadows: false"), std::string::npos) << "the writer never emitted the switch";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));
        Entity ground = FindByTag(*reloaded, "Lawn");
        ASSERT_TRUE(static_cast<bool>(ground));
        ASSERT_TRUE(ground.HasComponent<FoliageComponent>());
        const auto& foliage = ground.GetComponent<FoliageComponent>();
        ASSERT_EQ(foliage.m_Layers.Num(), 1);
        EXPECT_FALSE(foliage.m_Layers[0].CastShadows) << "a lawn authored out of the shadow maps came back casting";
        // And undo equality sees it, or an inspector toggle is unrevertable.
        FoliageLayer toggled = foliage.m_Layers[0];
        toggled.CastShadows = true;
        EXPECT_FALSE(toggled == foliage.m_Layers[0]);
    }

    TEST(FoliageShadowCasterSaveLoad, ASceneWithoutTheKeyStillCasts)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity ground = scene->CreateEntity("Meadow");
            ground.AddComponent<TerrainComponent>();
            ground.AddComponent<FoliageComponent>(MakeLawn(true));
            yaml = SceneSerializer(scene).SerializeToYAML();
        }
        // A scene written before the switch: the same YAML without its key.
        const std::string key = "CastShadows: true";
        const auto at = yaml.find(key);
        ASSERT_NE(at, std::string::npos);
        yaml.erase(at, key.size());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));
        Entity ground = FindByTag(*reloaded, "Meadow");
        ASSERT_TRUE(static_cast<bool>(ground));
        ASSERT_EQ(ground.GetComponent<FoliageComponent>().m_Layers.Num(), 1);
        EXPECT_TRUE(ground.GetComponent<FoliageComponent>().m_Layers[0].CastShadows)
            << "a scene authored before the switch lost its plants' shadows";
    }

    TEST(FoliageShadowCasterSaveLoad, SaveGameRoundTripsTheSwitchAndAnOlderSaveCasts)
    {
        // The current format carries it.
        {
            FoliageComponent authored = MakeLawn(false);
            std::vector<u8> bytes;
            {
                FMemoryWriter writer(bytes);
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                SaveGameComponentSerializer::Serialize(writer, authored);
            }
            FoliageComponent loaded;
            FMemoryReader reader(bytes);
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "the overload wrote more bytes than it reads back";
            ASSERT_EQ(loaded.m_Layers.Num(), 1);
            EXPECT_FALSE(loaded.m_Layers[0].CastShadows);
        }
        // A v39 save stops before the field, and loads casting -- even into a
        // layer object a v40 load left off, which the restore path reuses.
        {
            // A writer writes every field whatever version it is stamped with,
            // so the v39 payload is the current one without the switch: the
            // last two bools are the layer's CastShadows and the component's
            // m_Enabled, four bytes each. Checked, so a layout change fails
            // here instead of cutting the wrong bytes.
            FoliageComponent older = MakeLawn(false);
            std::vector<u8> bytes;
            {
                FMemoryWriter writer(bytes);
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                SaveGameComponentSerializer::Serialize(writer, older);
            }
            constexpr sizet kBool = 4;
            ASSERT_GE(bytes.size(), 2 * kBool);
            const std::vector<u8> tail(bytes.end() - 2 * kBool, bytes.end());
            ASSERT_EQ(tail, (std::vector<u8>{ 0, 0, 0, 0, 1, 0, 0, 0 }))
                << "the payload no longer ends in CastShadows (off) then m_Enabled (on)";
            bytes.erase(bytes.end() - 2 * kBool, bytes.end() - kBool);

            FoliageComponent reused = MakeLawn(false);
            FMemoryReader reader(bytes);
            reader.SetArchiveVersion(39);
            SaveGameComponentSerializer::Serialize(reader, reused);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "a v39 payload was not consumed exactly";
            ASSERT_EQ(reused.m_Layers.Num(), 1);
            EXPECT_TRUE(reused.m_Layers[0].CastShadows) << "a v39 save kept the previous load's switch";
        }
    }
} // namespace OloEngine::Tests
