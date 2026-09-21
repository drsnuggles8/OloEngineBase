// OLO_TEST_LAYER: unit
// =============================================================================
// Persistence for issue #1237's authored fields, across both surfaces they
// reach: scene YAML (hand-written keys next to the impostor band) and save
// games (a hand-written positional band at format v38).
//
// The back-compat question is the one that matters here, and it is asserted
// twice — once per surface. Every field defaults to the IDENTITY: no
// per-instance transition spread, no hysteresis, no stochastic coverage and no
// density reduction. So a scene or a save written before #1237 has to load with
// its plants handing over at exactly the distances that build handed over at.
// A default the other way would make every existing world change appearance on
// upgrade, under cover of a "smoother transitions" feature.
//
// The v37 test below writes the OLD positional layout by hand rather than
// through the production writer, so it tests the reader's version gate rather
// than the writer agreeing with itself — and `AtEnd()` is what catches a v38
// band that consumed bytes a v37 save does not contain.
#include "OloEnginePCH.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageLodTransition.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
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

        // Every field distinct from its default, and none of them a round
        // number a lower-precision lane would survive by luck.
        FoliageLayer MakeAuthoredLayer()
        {
            FoliageLayer l;
            l.Name = "Meadow";
            l.LodTransitionSpread = 13.40625f;
            l.LodHysteresis = 0.16406250f;
            l.LodStochasticCoverage = true;
            l.UseDensityLod = true;
            l.DensityLodStartDistance = 26.59375f;
            l.DensityLodEndDistance = 118.78125f;
            l.DensityLodMinFraction = 0.31640625f;
            l.DensityLodFadeFraction = 0.08203125f;
            l.DensityLodMaxScale = 2.703125f;
            return l;
        }

        void ExpectLodEqual(const FoliageLayer& got, const FoliageLayer& want)
        {
            EXPECT_TRUE(Math::BitwiseEqual(got.LodTransitionSpread, want.LodTransitionSpread));
            EXPECT_TRUE(Math::BitwiseEqual(got.LodHysteresis, want.LodHysteresis));
            EXPECT_EQ(got.LodStochasticCoverage, want.LodStochasticCoverage);
            EXPECT_EQ(got.UseDensityLod, want.UseDensityLod);
            EXPECT_TRUE(Math::BitwiseEqual(got.DensityLodStartDistance, want.DensityLodStartDistance));
            EXPECT_TRUE(Math::BitwiseEqual(got.DensityLodEndDistance, want.DensityLodEndDistance));
            EXPECT_TRUE(Math::BitwiseEqual(got.DensityLodMinFraction, want.DensityLodMinFraction));
            EXPECT_TRUE(Math::BitwiseEqual(got.DensityLodFadeFraction, want.DensityLodFadeFraction));
            EXPECT_TRUE(Math::BitwiseEqual(got.DensityLodMaxScale, want.DensityLodMaxScale));
        }

        // The v37 positional layout, written by hand. Everything up to and
        // including InteractionResponse, and nothing after it.
        void WriteV37Layer(FMemoryWriter& ar, FoliageLayer& l)
        {
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
            ar << l.SlopeFeather;
            ar << l.UseAltitudeBand << l.MinAltitude << l.MaxAltitude << l.AltitudeFeather;
            ar << l.UseMoisture << l.MinMoisture << l.MaxMoisture << l.MoistureFeather;
            ar << l.ExclusionSplatmapChannel << l.ExclusionThreshold;
            ar << l.ClumpStrength << l.ClumpScale << l.ClumpFalloff << l.ClumpScaleInfluence;
            ar << l.ClumpGroup;
            ar << l.GroundOffset << l.SlopeSinkFactor;
            ar << l.DecorrelatedVariation;
            ar << l.WindStiffness << l.WindBranchWeight << l.WindLeafWeight << l.WindDebugDisplacement;
            ar << l.InteractionResponse;
        }
    } // namespace

    TEST(FoliageLodTransitionSaveLoad, SceneYamlRoundTripsEveryAuthoredField)
    {
        const FoliageLayer authored = MakeAuthoredLayer();

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity ground = scene->CreateEntity("Meadow");
            ground.AddComponent<TerrainComponent>();
            ground.AddComponent<FoliageComponent>().m_Layers.Add(authored);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }
        ASSERT_FALSE(yaml.empty());
        EXPECT_NE(yaml.find("DensityLodMinFraction"), std::string::npos)
            << "the writer never emitted the key, so the round trip below would pass on defaults";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));
        Entity restored = FindByTag(*reloaded, "Meadow");
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<FoliageComponent>());
        const auto& layers = restored.GetComponent<FoliageComponent>().m_Layers;
        ASSERT_EQ(layers.Num(), 1u);
        ExpectLodEqual(layers[0], authored);

        // And undo equality sees every one of them, or an inspector edit is
        // unrevertable (SceneHierarchyPanel::DrawComponent's operator== tier).
        for (auto mutate : std::vector<void (*)(FoliageLayer&)>{ [](FoliageLayer& l)
                                                                 { l.LodTransitionSpread += 1.0f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.LodHysteresis += 0.01f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.LodStochasticCoverage = !l.LodStochasticCoverage; },
                                                                 [](FoliageLayer& l)
                                                                 { l.UseDensityLod = !l.UseDensityLod; },
                                                                 [](FoliageLayer& l)
                                                                 { l.DensityLodStartDistance += 1.0f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.DensityLodEndDistance += 1.0f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.DensityLodMinFraction += 0.01f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.DensityLodFadeFraction += 0.01f; },
                                                                 [](FoliageLayer& l)
                                                                 { l.DensityLodMaxScale += 0.01f; } })
        {
            FoliageLayer mutated = layers[0];
            mutate(mutated);
            EXPECT_FALSE(mutated == layers[0]) << "operator== is missing one of the #1237 fields";
        }
    }

    TEST(FoliageLodTransitionSaveLoad, ASceneWithoutTheKeysLoadsWithTheFeatureInert)
    {
        // The prior-on-disk-version cell. A scene authored before #1237 names
        // none of these keys; it must come back with the whole feature off,
        // not with a plausible-looking default that changes its image.
        auto scene = Scene::Create();
        const std::string yaml = R"(Scene: Untitled
Entities:
  - Entity: 1234567890123456
    TagComponent:
      Tag: Meadow
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    FoliageComponent:
      Enabled: true
      Layers:
        - Name: Grass
          Density: 1
          ViewDistance: 100
          FadeStartDistance: 80
)";
        ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml));
        Entity ground = FindByTag(*scene, "Meadow");
        ASSERT_TRUE(static_cast<bool>(ground));
        ASSERT_TRUE(ground.HasComponent<FoliageComponent>());
        const auto& layers = ground.GetComponent<FoliageComponent>().m_Layers;
        ASSERT_EQ(layers.Num(), 1u);
        ExpectLodEqual(layers[0], FoliageLayer{});
    }

    TEST(FoliageLodTransitionSaveLoad, HostileSceneValuesAreBounded)
    {
        auto scene = Scene::Create();
        const std::string yaml = R"(Scene: Untitled
Entities:
  - Entity: 1234567890123456
    TagComponent:
      Tag: Meadow
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    FoliageComponent:
      Enabled: true
      Layers:
        - Name: Grass
          LodTransitionSpread: .nan
          LodHysteresis: 9000
          UseDensityLod: true
          DensityLodStartDistance: -50
          DensityLodEndDistance: -900
          DensityLodMinFraction: .nan
          DensityLodFadeFraction: 17
          DensityLodMaxScale: .inf
)";
        ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml));
        const auto& l = FindByTag(*scene, "Meadow").GetComponent<FoliageComponent>().m_Layers[0];

        EXPECT_TRUE(std::isfinite(l.LodTransitionSpread));
        EXPECT_GE(l.LodTransitionSpread, 0.0f);
        EXPECT_LE(l.LodHysteresis, 0.5f);
        EXPECT_GE(l.DensityLodStartDistance, 0.0f);
        EXPECT_GE(l.DensityLodEndDistance, l.DensityLodStartDistance)
            << "an inverted band makes the keep-fraction smoothstep undefined";
        EXPECT_GE(l.DensityLodMinFraction, FoliageLod::kMinKeepFraction);
        EXPECT_LE(l.DensityLodFadeFraction, 1.0f);
        EXPECT_TRUE(std::isfinite(l.DensityLodMaxScale));
        EXPECT_GE(l.DensityLodMaxScale, 1.0f);
    }

    TEST(FoliageLodTransitionSaveLoad, SaveGameRoundTripsEveryFieldAndSanitizesHostileBytes)
    {
        FoliageComponent authored;
        authored.m_Layers.Add(MakeAuthoredLayer());

        std::vector<u8> bytes;
        {
            FMemoryWriter writer(bytes);
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(writer, authored);
        }

        FoliageComponent loaded;
        {
            FMemoryReader reader(bytes);
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "the reader and the writer disagree about the v38 band's length";
        }
        ASSERT_EQ(loaded.m_Layers.Num(), 1u);
        ExpectLodEqual(loaded.m_Layers[0], authored.m_Layers[0]);

        // A save file is no more trusted than a .olo: the same bounds.
        FoliageComponent hostile;
        {
            FoliageLayer l;
            l.UseDensityLod = true;
            l.LodTransitionSpread = std::numeric_limits<f32>::quiet_NaN();
            l.LodHysteresis = 12.0f;
            l.DensityLodStartDistance = -4.0f;
            l.DensityLodEndDistance = std::numeric_limits<f32>::infinity();
            l.DensityLodMinFraction = std::numeric_limits<f32>::quiet_NaN();
            l.DensityLodFadeFraction = 42.0f;
            l.DensityLodMaxScale = -3.0f;
            hostile.m_Layers.Add(l);
        }
        std::vector<u8> poison;
        {
            FMemoryWriter writer(poison);
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(writer, hostile);
        }
        FoliageComponent recovered;
        FMemoryReader reader(poison);
        reader.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(reader, recovered);
        ASSERT_EQ(recovered.m_Layers.Num(), 1u);
        const auto& r = recovered.m_Layers[0];
        EXPECT_TRUE(std::isfinite(r.LodTransitionSpread));
        EXPECT_LE(r.LodHysteresis, 0.5f);
        EXPECT_GE(r.DensityLodStartDistance, 0.0f);
        EXPECT_TRUE(std::isfinite(r.DensityLodEndDistance));
        EXPECT_GE(r.DensityLodEndDistance, r.DensityLodStartDistance);
        EXPECT_GE(r.DensityLodMinFraction, FoliageLod::kMinKeepFraction);
        EXPECT_LE(r.DensityLodFadeFraction, 1.0f);
        EXPECT_GE(r.DensityLodMaxScale, 1.0f);
    }

    TEST(FoliageLodTransitionSaveLoad, AV37SaveLoadsInertAndConsumesItsExactPayload)
    {
        std::vector<u8> bytes;
        {
            FMemoryWriter ar(bytes);
            FoliageLayer l;
            l.MeshViewDistance = 44.0f; // a pre-#1237 field, to prove the walk landed
            u32 count = 1;
            ar << count;
            WriteV37Layer(ar, l);
            bool enabled = true;
            ar << enabled;
        }

        FoliageComponent loaded;
        FMemoryReader reader(bytes);
        reader.SetArchiveVersion(37);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "the v38 band consumed bytes a v37 save does not contain";
        ASSERT_EQ(loaded.m_Layers.Num(), 1u);
        EXPECT_FLOAT_EQ(loaded.m_Layers[0].MeshViewDistance, 44.0f);
        ExpectLodEqual(loaded.m_Layers[0], FoliageLayer{});
        EXPECT_TRUE(loaded.m_Enabled);
    }
} // namespace OloEngine::Tests
