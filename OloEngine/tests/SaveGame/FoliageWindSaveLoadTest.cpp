// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/FileStream.h"
#include "../TestTempDir.h"
#include <gtest/gtest.h>
#include <limits>
#include <fstream>
#include <sstream>

namespace OloEngine::Tests
{
    namespace
    {
        FoliageComponent Read(const std::vector<u8>& bytes, u32 version)
        {
            FoliageComponent loaded;
            FMemoryReader reader(bytes);
            reader.SetArchiveVersion(version);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd());
            return loaded;
        }

        std::vector<u8> Write(FoliageComponent component)
        {
            std::vector<u8> bytes;
            FMemoryWriter writer(bytes);
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(writer, component);
            return bytes;
        }

        // Explicit old positional layout, independent of the production writer.
        std::vector<u8> WritePrior(u32 version)
        {
            std::vector<u8> bytes;
            FMemoryWriter ar(bytes);
            FoliageLayer l;
            u32 count = 1;
            ar << count;
            ar << l.Name << l.MeshPath << l.AlbedoPath;
            ar << l.Density << l.SplatmapChannel << l.MinSlopeAngle << l.MaxSlopeAngle;
            ar << l.MinScale << l.MaxScale << l.MinHeight << l.MaxHeight << l.RandomRotation;
            ar << l.ViewDistance << l.FadeStartDistance << l.WindStrength << l.WindSpeed;
            ar << l.BaseColor << l.Roughness << l.AlphaCutoff << l.Enabled;
            ar << l.UseImpostor << l.ImpostorStartDistance << l.ImpostorTransitionBand;
            ar << l.ImpostorFramesPerAxis << l.ImpostorAtlasResolution << l.ImpostorHemiOctahedral;
            ar << l.UseAuthoredMesh << l.MeshViewDistance << l.MeshFadeStartDistance;
            ar << l.NormalMapPath << l.RoughnessMapPath << l.ThicknessMapPath;
            ar << l.NormalStrength << l.TransmissionStrength << l.TransmissionColor << l.Thickness;
            ar << l.TransmissionDistortion << l.TransmissionPower << l.TransmissionWrap << l.TransmissionAmbient;
            if (version == 35)
            {
                l.SlopeFeather = 7.5f;
                l.UseMoisture = true;
                l.ClumpStrength = 0.625f;
                l.ClumpGroup = 3;
                l.DecorrelatedVariation = true;
                ar << l.SlopeFeather;
                ar << l.UseAltitudeBand << l.MinAltitude << l.MaxAltitude << l.AltitudeFeather;
                ar << l.UseMoisture << l.MinMoisture << l.MaxMoisture << l.MoistureFeather;
                ar << l.ExclusionSplatmapChannel << l.ExclusionThreshold;
                ar << l.ClumpStrength << l.ClumpScale << l.ClumpFalloff << l.ClumpScaleInfluence;
                ar << l.ClumpGroup;
                ar << l.GroundOffset << l.SlopeSinkFactor;
                ar << l.DecorrelatedVariation;
            }
            bool enabled = true;
            ar << enabled;
            return bytes;
        }
    } // namespace

    TEST(FoliageWindSaveLoad, CurrentRoundTripPreservesAuthoredWeightsAndUndoEquality)
    {
        FoliageComponent authored;
        FoliageLayer layer;
        layer.WindStiffness = 0.375f;
        layer.WindBranchWeight = 0.625f;
        layer.WindLeafWeight = 0.8125f;
        layer.WindDebugDisplacement = true;
        layer.UseMoisture = true;
        layer.ClumpStrength = 0.5f;
        layer.DecorrelatedVariation = true;
        authored.m_Layers.push_back(layer);
        const auto loaded = Read(Write(authored), kSaveGameFormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        EXPECT_TRUE(loaded.m_Layers[0] == layer);
        layer.WindLeafWeight = 0.0f;
        EXPECT_FALSE(loaded.m_Layers[0] == layer);
    }

    TEST(FoliageWindSaveLoad, V34KeepsLegacyWindAndConsumesExactPayload)
    {
        const auto path = TempFile("prior-v34.olosave");
        SaveGameHeader header;
        header.FormatVersion = 34;
        ASSERT_TRUE(SaveGameFile::Write(path, header, SaveGameMetadata{}, {}, WritePrior(34)));
        SaveGameHeader loadedHeader;
        ASSERT_TRUE(SaveGameFile::ReadHeader(path, loadedHeader));
        ASSERT_EQ(loadedHeader.FormatVersion, 34u);
        ASSERT_TRUE(SaveGameFile::ValidateChecksum(path));
        std::vector<u8> payload;
        ASSERT_TRUE(SaveGameFile::ReadPayload(path, payload));
        const auto loaded = Read(payload, loadedHeader.FormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindStiffness, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindBranchWeight, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindLeafWeight, 0.0f);
        EXPECT_FALSE(loaded.m_Layers[0].WindDebugDisplacement);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindStrength, 0.3f);
    }

    TEST(FoliageWindSaveLoad, V35PreservesHabitatAndDefaultsWindWithoutConsumingFollowingFields)
    {
        const auto path = TempFile("prior-v35.olosave");
        SaveGameHeader header;
        header.FormatVersion = 35;
        ASSERT_TRUE(SaveGameFile::Write(path, header, SaveGameMetadata{}, {}, WritePrior(35)));
        SaveGameHeader loadedHeader;
        ASSERT_TRUE(SaveGameFile::ReadHeader(path, loadedHeader));
        ASSERT_EQ(loadedHeader.FormatVersion, 35u);
        ASSERT_TRUE(SaveGameFile::ValidateChecksum(path));
        std::vector<u8> payload;
        ASSERT_TRUE(SaveGameFile::ReadPayload(path, payload));
        const auto loaded = Read(payload, loadedHeader.FormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        const auto& layer = loaded.m_Layers[0];
        EXPECT_FLOAT_EQ(layer.SlopeFeather, 7.5f);
        EXPECT_TRUE(layer.UseMoisture);
        EXPECT_FLOAT_EQ(layer.ClumpStrength, 0.625f);
        EXPECT_EQ(layer.ClumpGroup, 3u);
        EXPECT_TRUE(layer.DecorrelatedVariation);
        EXPECT_FLOAT_EQ(layer.WindStiffness, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindBranchWeight, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindLeafWeight, 0.0f);
        EXPECT_FALSE(layer.WindDebugDisplacement);
        EXPECT_TRUE(loaded.m_Enabled);
    }

    TEST(FoliageWindSaveLoad, NonFiniteAndOutOfRangeWeightsAreSanitized)
    {
        FoliageComponent authored;
        FoliageLayer layer;
        layer.WindStiffness = std::numeric_limits<f32>::quiet_NaN();
        layer.WindBranchWeight = -1.0f;
        layer.WindLeafWeight = 3.0f;
        authored.m_Layers.push_back(layer);
        const auto loaded = Read(Write(authored), kSaveGameFormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindStiffness, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindBranchWeight, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].WindLeafWeight, 1.0f);
    }

    TEST(FoliageWindSaveLoad, SceneYamlAndCookedSceneBlobPreserveWind)
    {
        auto scene = Scene::Create();
        auto entity = scene->CreateEntity("WindPlant");
        auto& foliage = entity.AddComponent<FoliageComponent>();
        FoliageLayer layer;
        layer.WindStiffness = 0.375f;
        layer.WindBranchWeight = 0.625f;
        layer.WindLeafWeight = 0.8125f;
        layer.WindDebugDisplacement = true;
        foliage.m_Layers.push_back(layer);
        const auto yaml = SceneSerializer(scene).SerializeToYAML();
        auto restored = Scene::Create();
        ASSERT_TRUE(SceneSerializer(restored).DeserializeFromYAML(yaml));
        const auto check = [&layer](Ref<Scene> result)
        {
            sizet found = 0;
            for (auto id : result->GetAllEntitiesWith<FoliageComponent>())
            {
                Entity e{ id, result.get() };
                const auto& layers = e.GetComponent<FoliageComponent>().m_Layers;
                ASSERT_EQ(layers.size(), 1u);
                EXPECT_TRUE(layers[0] == layer);
                ++found;
            }
            EXPECT_EQ(found, 1u);
        };
        check(restored);
        // Exercise the actual cook writer and runtime scene reader.
        ASSERT_TRUE(Project::HasAssetManager() || !Project::GetActive());
        struct AssetManagerRestore
        {
            Ref<AssetManagerBase> Previous;
            ~AssetManagerRestore()
            {
                if (Previous)
                    Project::SetAssetManager(Previous);
                else
                    Project::Unload();
            }
        } restore{ Project::HasAssetManager() ? Project::GetAssetManager() : nullptr };
        auto manager = Ref<RuntimeAssetManager>::Create(false);
        Project::SetAssetManager(manager);
        const auto handle = AssetManager::AddMemoryOnlyAsset(scene);
        ASSERT_TRUE(handle);
        const auto path = TempFile("foliage-wind-scene.bin");
        SceneAssetSerializer serializer;
        AssetSerializationInfo cooked{};
        {
            FileStreamWriter writer(path);
            ASSERT_TRUE(writer.IsStreamGood());
            ASSERT_TRUE(serializer.SerializeToAssetPack(handle, writer, cooked));
        }
        EXPECT_GT(cooked.Size, yaml.size());
        FileStreamReader reader(path);
        AssetPackFile::SceneInfo info;
        info.Handle = handle;
        info.PackedOffset = cooked.Offset;
        info.PackedSize = cooked.Size;
        auto packed = serializer.DeserializeSceneFromAssetPack(reader, info);
        ASSERT_TRUE(packed);
        check(packed);
    }

    TEST(FoliageWindSaveLoad, SceneFuzzSeedCannotPublishNonFiniteWind)
    {
        // __FILE__ can be remapped by sanitizer builds; use the configured source location.
        const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path() / "OloEngine/tests/Fuzzing/corpus/scene_yaml/foliage_wind_weights.yaml";
        std::ifstream input(path);
        ASSERT_TRUE(input.good());
        std::ostringstream yaml;
        yaml << input.rdbuf();
        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml.str()));
        const auto ids = scene->GetAllEntitiesWith<FoliageComponent>();
        ASSERT_EQ(ids.size(), 1u);
        Entity entity{ *ids.begin(), scene.get() };
        const auto& layer = entity.GetComponent<FoliageComponent>().m_Layers.at(0);
        EXPECT_FLOAT_EQ(layer.WindStrength, 0.3f);
        EXPECT_FLOAT_EQ(layer.WindSpeed, 1.0f);
        EXPECT_FLOAT_EQ(layer.WindStiffness, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindBranchWeight, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindLeafWeight, 1.0f);
        EXPECT_TRUE(layer.WindDebugDisplacement);
    }

    TEST(FoliageWindSaveLoad, PriorSceneLoadsLegacyDefaults)
    {
        const std::string yaml = R"(Scene: PriorWind
Entities:
  - Entity: 1236
    TagComponent:
      Tag: Prior
    FoliageComponent:
      Enabled: true
      Layers:
        - Name: Grass
          WindStrength: 0.3
          WindSpeed: 1.0
)";
        auto scene = Scene::Create();
        const auto path = TempFile("prior-wind.olo");
        {
            std::ofstream output(path);
            output << yaml;
        }
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path.string()));
        const auto ids = scene->GetAllEntitiesWith<FoliageComponent>();
        ASSERT_EQ(ids.size(), 1u);
        Entity entity{ *ids.begin(), scene.get() };
        const auto& layer = entity.GetComponent<FoliageComponent>().m_Layers[0];
        EXPECT_FLOAT_EQ(layer.WindStiffness, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindBranchWeight, 0.0f);
        EXPECT_FLOAT_EQ(layer.WindLeafWeight, 0.0f);
    }
} // namespace OloEngine::Tests
