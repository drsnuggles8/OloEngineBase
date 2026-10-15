// OLO_TEST_LAYER: unit
// =============================================================================
// GroomWetnessTest.cpp — the wet coat's math and its authoring (#1615).
//
// The curve a coat wets and dries along, the response it shades with, the way
// a wet strand lies down, and GroomWetnessComponent through every format it is
// stored in. The shaders' twins are held to these functions by
// GroomWetnessShaderTest; the deformation buffer's by GroomGpuDeformationTest.
// =============================================================================
#include "OloEnginePCH.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomWetness.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

        GroomWetnessFrame Soaked(f32 line, f32 band = 0.04f, f32 rain = 0.0f)
        {
            GroomWetnessFrame frame;
            frame.Line = line;
            frame.Band = band;
            frame.Rain = rain;
            return frame;
        }
    } // namespace

    // ── The waterline ────────────────────────────────────────────────────────

    TEST(GroomWetness, ACoatIsSoakedBelowItsLineDryAboveAndHalfWetOnIt)
    {
        const GroomWetnessFrame frame = Soaked(0.5f, 0.04f);
        EXPECT_FLOAT_EQ(GroomWetnessAt(0.3f, frame), 1.0f) << "a leg under the line";
        EXPECT_FLOAT_EQ(GroomWetnessAt(0.9f, frame), 0.0f) << "a back above it";
        EXPECT_NEAR(GroomWetnessAt(0.5f, frame), 0.5f, 1.0e-6f) << "the line is the middle of the edge";
        // Monotone through the band: legs get wet before the back.
        f32 previous = 2.0f;
        for (f32 h = 0.44f; h <= 0.56f; h += 0.005f)
        {
            const f32 w = GroomWetnessAt(h, frame);
            EXPECT_LE(w, previous) << "height " << h;
            previous = w;
        }
        EXPECT_FLOAT_EQ(GroomWetnessAt(0.54f, frame), 0.0f) << "the edge ends a band above the line";
        EXPECT_FLOAT_EQ(GroomWetnessAt(0.46f, frame), 1.0f) << "and a band below it";
    }

    TEST(GroomWetness, RainWetsTheWholeCoatAndNeverDriesBelowTheWaterline)
    {
        const GroomWetnessFrame frame = Soaked(0.5f, 0.04f, 0.35f);
        EXPECT_FLOAT_EQ(GroomWetnessAt(2.0f, frame), 0.35f) << "rain reaches the back";
        EXPECT_FLOAT_EQ(GroomWetnessAt(0.1f, frame), 1.0f) << "and the water's soak wins below the line";
        EXPECT_FLOAT_EQ(GroomWetnessAt(kNaN, frame), 0.35f) << "an unknown height is the rain's";
        EXPECT_FALSE(GroomWetnessFrame{}.Any()) << "the default frame is dry";
        EXPECT_TRUE(Soaked(0.0f).Any());
    }

    // ── The cycle: dry -> wet -> drying ─────────────────────────────────────

    TEST(GroomWetness, AWadeSoaksUpToTheWaterAndDryingRecedesTheLineToTheFeet)
    {
        GroomWetnessState state;
        GroomWetnessInputs in;
        in.Bottom = -0.05f;
        in.SoakRate = 2.0f;
        in.DryRate = 0.25f;
        in.DeltaSeconds = 0.1f;
        EXPECT_FLOAT_EQ(GroomWetnessLine(state, in.Bottom), GroomWetnessLimits::DryLine) << "a fresh coat is dry";

        // Wading to 0.6 m: the soak wicks up from the feet to the water.
        in.Waterline = 0.6f;
        f32 previous = in.Bottom;
        for (u32 tick = 0; tick < 5u; ++tick)
        {
            AdvanceGroomWetness(state, in);
            const f32 line = GroomWetnessLine(state, in.Bottom);
            EXPECT_GT(line, previous) << "tick " << tick;
            EXPECT_LE(line, 0.6f + 1.0e-6f) << "never above the water";
            previous = line;
        }
        for (u32 tick = 0; tick < 10u; ++tick)
        {
            AdvanceGroomWetness(state, in);
        }
        EXPECT_FLOAT_EQ(state.Immersion, 1.0f);
        EXPECT_NEAR(GroomWetnessLine(state, in.Bottom), 0.6f, 1.0e-5f) << "soaked to the water line";

        // Out of the water: the line recedes, back first, legs last.
        in.Waterline.reset();
        previous = GroomWetnessLine(state, in.Bottom);
        for (u32 tick = 0; tick < 20u; ++tick)
        {
            AdvanceGroomWetness(state, in);
            const f32 line = GroomWetnessLine(state, in.Bottom);
            EXPECT_LT(line, previous) << "tick " << tick;
            previous = line;
        }
        EXPECT_GT(GroomWetnessLine(state, in.Bottom), in.Bottom) << "halfway dry, the legs are still wet";
        for (u32 tick = 0; tick < 40u; ++tick)
        {
            AdvanceGroomWetness(state, in);
        }
        EXPECT_FLOAT_EQ(state.Immersion, 0.0f);
        EXPECT_FLOAT_EQ(GroomWetnessLine(state, in.Bottom), GroomWetnessLimits::DryLine) << "dry again";
    }

    TEST(GroomWetness, WadingBackInAtTheKneesKeepsAWetBackAndTheLineIsContinuous)
    {
        GroomWetnessState state;
        GroomWetnessInputs in;
        in.Bottom = 0.0f;
        in.SoakRate = 100.0f;
        in.DryRate = 0.1f;
        in.DeltaSeconds = 1.0f;
        in.Waterline = 1.2f;
        AdvanceGroomWetness(state, in); // soaked to 1.2 m in one tick
        in.Waterline.reset();
        AdvanceGroomWetness(state, in); // dried to 90 %: the line at 1.08 m
        const f32 before = GroomWetnessLine(state, in.Bottom);
        EXPECT_NEAR(before, 1.08f, 1.0e-5f);
        in.Waterline = 0.4f;
        in.DeltaSeconds = 1.0e-4f;
        AdvanceGroomWetness(state, in);
        EXPECT_NEAR(GroomWetnessLine(state, in.Bottom), before, 1.0e-4f)
            << "stepping into shallower water neither drops nor raises the line";
        in.DeltaSeconds = 1.0f;
        AdvanceGroomWetness(state, in);
        EXPECT_NEAR(GroomWetnessLine(state, in.Bottom), before, 1.0e-4f)
            << "and the soak heads for the higher of the two, not the water's knee height";
    }

    TEST(GroomWetness, RainSoaksAtTheSoakRateDriesAtTheDryRateAndIgnoresNonsense)
    {
        GroomWetnessState state;
        GroomWetnessInputs in;
        in.SoakRate = 0.5f;
        in.DryRate = 0.1f;
        in.DeltaSeconds = 1.0f;
        in.RainTarget = 0.8f;
        AdvanceGroomWetness(state, in);
        EXPECT_FLOAT_EQ(state.Rain, 0.5f);
        AdvanceGroomWetness(state, in);
        EXPECT_FLOAT_EQ(state.Rain, 0.8f);
        in.RainTarget = 0.0f;
        AdvanceGroomWetness(state, in);
        EXPECT_NEAR(state.Rain, 0.7f, 1.0e-6f);

        const GroomWetnessState saved = state;
        in.DeltaSeconds = kNaN;
        AdvanceGroomWetness(state, in);
        EXPECT_FLOAT_EQ(state.Rain, saved.Rain) << "a NaN tick changes nothing";
        in.DeltaSeconds = 1.0f;
        in.SoakRate = kNaN;
        in.RainTarget = kNaN;
        AdvanceGroomWetness(state, in);
        EXPECT_TRUE(std::isfinite(state.Rain) && std::isfinite(state.Immersion));
    }

    TEST(GroomWetness, TheCoatMeasuresHeightAboveItsOriginHoweverItIsTurned)
    {
        // The dog's coat: parented to a body turned a quarter about Z, so the
        // coat's own +Y is level -- and at half scale, at (2, 0.3, -1).
        const glm::mat4 world =
            glm::rotate(glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.3f, -1.0f)), glm::vec3(0.5f)),
                        glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const GroomWetnessAxis axis = MakeGroomWetnessAxis(world);
        for (const glm::vec3 p : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.3f, -0.2f, 0.7f) })
        {
            EXPECT_NEAR((world * glm::vec4(p, 1.0f)).y, 0.3f + glm::dot(p, axis.Height), 1.0e-6f)
                << "an object point stands dot(p, Height) above the origin";
        }
        const auto line = GroomWetnessLocalHeight(axis, 0.8f);
        ASSERT_TRUE(line.has_value());
        EXPECT_NEAR(*line, 0.5f, 1.0e-6f) << "water at 0.8 m reaches 0.5 m above a coat standing at 0.3 m";
        EXPECT_FALSE(GroomWetnessLocalHeight(axis, kNaN).has_value());
        EXPECT_NEAR(GroomWetnessLowest(axis, glm::vec3(-1.0f), glm::vec3(1.0f)), -0.5f, 1.0e-6f)
            << "the box's lowest corner, measured the same way";
        const glm::vec3 gravity = GroomWetGravityObject(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1)));
        EXPECT_NEAR(gravity.x, -1.0f, 1.0e-5f) << "a coat turned a quarter about Z feels gravity along its -X";
        EXPECT_NEAR(glm::length(gravity), 1.0f, 1.0e-5f);
    }

    TEST(GroomWetness, AWetGuideIsHeavierAndADryOneIsAsAuthored)
    {
        EXPECT_EQ(GroomWetGuideStiffness(0.0f), 1.0f) << "dry is the authored spring, exactly";
        EXPECT_FLOAT_EQ(GroomWetGuideStiffness(1.0f), 0.6f);
        EXPECT_FLOAT_EQ(GroomWetGuideStiffness(0.5f), 0.8f);
        EXPECT_FLOAT_EQ(GroomWetGuideStiffness(4.0f), 0.6f);
        EXPECT_EQ(GroomWetGuideStiffness(kNaN), 1.0f) << "an unknown wetness changes nothing";
    }

    // ── The response ─────────────────────────────────────────────────────────

    TEST(GroomWetness, AWetFibreAbsorbsMoreIsSmootherAndNarrowerAndDryIsTheIdentity)
    {
        const GroomWetResponse response;
        const GroomWetFibreScales dry = GroomWetFibre(0.0f, response);
        EXPECT_FLOAT_EQ(dry.SigmaA, 1.0f);
        EXPECT_FLOAT_EQ(dry.V0, 1.0f);
        EXPECT_FLOAT_EQ(dry.S, 1.0f);
        const GroomWetFibreScales wet = GroomWetFibre(1.0f, response);
        EXPECT_FLOAT_EQ(wet.SigmaA, 2.2f) << "the issue's power curve of 2.2";
        EXPECT_FLOAT_EQ(wet.V0, 0.25f) << "half the roughness is a quarter of the variance";
        EXPECT_FLOAT_EQ(wet.S, 0.5f);
        EXPECT_FLOAT_EQ(wet.AlbedoPower, wet.SigmaA);
        // An albedo of exp(-c sigma) under sigma * k is the albedo to the k:
        // darker, and more saturated -- the ratio of two channels grows.
        const f32 c = 0.7f;
        const glm::vec3 sigma(0.4f, 0.9f, 1.6f);
        const glm::vec3 dryAlbedo = glm::exp(-c * sigma);
        const glm::vec3 wetAlbedo = glm::exp(-c * sigma * wet.SigmaA);
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(wetAlbedo[i], std::pow(dryAlbedo[i], wet.AlbedoPower), 1.0e-6f);
        EXPECT_GT(wetAlbedo.r / wetAlbedo.b, dryAlbedo.r / dryAlbedo.b) << "wet is more saturated";
        const GroomWetFibreScales half = GroomWetFibre(0.5f, response);
        EXPECT_FLOAT_EQ(half.SigmaA, 1.6f);
        EXPECT_FLOAT_EQ(GroomWetFibre(kNaN, response).SigmaA, 1.0f) << "unknown wetness is dry";
    }

    TEST(GroomWetness, AWetStrandLiesDownKeepsItsRootAndShortensByItsShare)
    {
        GroomWetResponse response;
        response.Clump = 0.0f;
        const glm::vec3 up(0.002f, 0.0f, 0.03f);        // a strand standing off its skin
        const glm::vec3 backGravity(0.0f, 0.0f, -1.0f); // gravity presses into the back
        EXPECT_EQ(GroomWetLocal(up, 1.0f, 0.0f, backGravity, glm::vec3(0.0f), response), up) << "dry is the identity";
        EXPECT_EQ(GroomWetLocal(glm::vec3(0.0f), 0.0f, 1.0f, backGravity, glm::vec3(0.0f), response), glm::vec3(0.0f))
            << "the root does not move";

        const glm::vec3 laid = GroomWetLocal(up, 1.0f, 1.0f, backGravity, glm::vec3(0.0f), response);
        EXPECT_LT(laid.z, up.z * 0.6f) << "on the back the strand lies down";
        EXPECT_GT(laid.z, 0.0f) << "and stays off the skin";
        EXPECT_NEAR(glm::length(laid), glm::length(up) * (1.0f - response.Shorten), 1.0e-6f)
            << "keeping its reach, less what it shortens by";

        const glm::vec3 bellyGravity(0.0f, 0.0f, 1.0f); // gravity leaves the belly
        const glm::vec3 hung = GroomWetLocal(glm::vec3(0.02f, 0.0f, 0.01f), 1.0f, 1.0f, bellyGravity, glm::vec3(0.0f), response);
        EXPECT_GT(hung.z / glm::length(hung), 0.01f / glm::length(glm::vec3(0.02f, 0.0f, 0.01f)))
            << "on the belly it hangs off the skin along gravity";

        const glm::vec3 flankGravity(0.0f, -1.0f, 0.0f); // the slope runs down -Y
        const glm::vec3 combed = GroomWetLocal(glm::vec3(0.02f, 0.0f, 0.02f), 1.0f, 1.0f, flankGravity, glm::vec3(0.0f), response);
        EXPECT_LT(combed.y, -0.005f) << "on a flank it is combed downhill";

        // More on what faces up: the same strand lies further on the back than
        // on a vertical flank.
        const glm::vec3 onFlank = GroomWetLocal(up, 1.0f, 1.0f, flankGravity, glm::vec3(0.0f), response);
        EXPECT_LT(laid.z, onFlank.z);
    }

    TEST(GroomWetness, AWetTipGathersTowardItsClumpQuadraticallyAndANullClumpGathersNothing)
    {
        GroomWetResponse response;
        response.Flatten = 0.0f;
        response.Shorten = 0.0f;
        response.Clump = 1.0f;
        const glm::vec3 gravity(0.0f, 0.0f, -1.0f);
        const glm::vec3 tip(0.01f, 0.0f, 0.03f);
        const glm::vec3 growth(-0.01f, 0.0f, 0.03f);
        EXPECT_NEAR(glm::length(GroomWetLocal(tip, 1.0f, 1.0f, gravity, growth, response) - growth), 0.0f, 1.0e-6f)
            << "a full clump puts the tip on its clump's growth";
        const glm::vec3 mid = GroomWetLocal(tip * 0.5f, 0.5f, 1.0f, gravity, growth, response);
        EXPECT_NEAR(glm::length(mid - (tip * 0.5f + ((growth * 0.5f) - (tip * 0.5f)) * 0.25f)), 0.0f, 1.0e-6f)
            << "halfway up, a quarter of the way: t squared";
        EXPECT_EQ(GroomWetLocal(tip, 1.0f, 1.0f, gravity, glm::vec3(0.0f), response), tip)
            << "no clump record, no pull -- not a pull to the root";
    }

    TEST(GroomWetness, TheDeformRecordIsDryUnlessSomeOfTheCoatIsWetAndClampsItsResponse)
    {
        const GroomDeformWetRecord dry = MakeGroomDeformWetRecord(GroomWetnessFrame{}, GroomWetResponse{}, glm::vec3(0.0f, -2.0f, 0.0f));
        EXPECT_FALSE(dry.Frame.w > 0.5f);
        EXPECT_FLOAT_EQ(dry.Gravity.y, -1.0f) << "gravity is a direction";
        GroomWetResponse wild;
        wild.Flatten = 5.0f;
        wild.Shorten = kNaN;
        wild.Clump = -1.0f;
        const GroomDeformWetRecord wet = MakeGroomDeformWetRecord(Soaked(0.2f, 9.0f), wild, glm::vec3(0.0f));
        EXPECT_TRUE(wet.Frame.w > 0.5f);
        EXPECT_FLOAT_EQ(wet.Frame.z, GroomWetnessLimits::MaxBand);
        EXPECT_FLOAT_EQ(wet.Response.x, 1.0f);
        EXPECT_FLOAT_EQ(wet.Response.y, 0.0f);
        EXPECT_FLOAT_EQ(wet.Response.z, 0.0f);
        EXPECT_FLOAT_EQ(wet.Gravity.y, -1.0f) << "a zero gravity falls back to world down";

        // The record's own wetness is the ROOT's: a root under the line wets
        // the strand, one above it leaves the strand dry.
        const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 local(0.0f, 0.0f, 0.03f);
        EXPECT_NE(GroomDeformWetLocal(wet, glm::vec3(0.0f, 0.1f, 0.0f), identity, glm::vec3(0.0f), local, 1.0f), local);
        EXPECT_EQ(GroomDeformWetLocal(wet, glm::vec3(0.0f, 9.0f, 0.0f), identity, glm::vec3(0.0f), local, 1.0f), local);
    }

    // ── The component, in every format it is stored in ──────────────────────

    namespace
    {
        GroomWetnessComponent AuthoredWetness()
        {
            GroomWetnessComponent c;
            c.m_Enabled = false;
            c.m_AutomaticWaterline = false;
            c.m_Waterline = 0.37f;
            c.m_Band = 0.07f;
            c.m_RainSoak = 0.25f;
            c.m_SoakRate = 3.0f;
            c.m_DryRate = 0.05f;
            c.m_Darkening = 3.1f;
            c.m_Gloss = 0.4f;
            c.m_Sheen = 0.3f;
            c.m_Flatten = 0.8f;
            c.m_Shorten = 0.2f;
            c.m_Clump = 0.7f;
            c.m_Immersion = 0.6f;
            c.m_SoakedLine = 0.9f;
            c.m_Rain = 0.15f;
            return c;
        }
    } // namespace

    TEST(GroomWetnessSerialization, SceneYamlKeepsEveryFieldAndTheState)
    {
        auto scene = Scene::Create();
        auto entity = scene->CreateEntity("Wet coat");
        entity.AddComponent<GroomWetnessComponent>() = AuthoredWetness();
        const auto yaml = SceneSerializer(scene).SerializeToYAML();
        auto loaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(yaml));
        const auto coat = loaded->FindEntityByName("Wet coat");
        ASSERT_TRUE(coat && coat.HasComponent<GroomWetnessComponent>());
        EXPECT_TRUE(coat.GetComponent<GroomWetnessComponent>() == AuthoredWetness());
    }

    TEST(GroomWetnessSerialization, MissingFieldsAreTheDefaultsAndOutOfRangeOnesAreClamped)
    {
        auto loaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(
            "Scene: Wet\nVersion: 1\nEntities:\n  - Entity: 1615\n    TagComponent:\n      Tag: Coat\n    GroomWetnessComponent: {}\n"
            "  - Entity: 1616\n    TagComponent:\n      Tag: Wild\n    GroomWetnessComponent:\n      Band: 50\n      Darkening: 0.2\n"
            "      Immersion: 7\n"));
        const auto coat = loaded->FindEntityByName("Coat");
        ASSERT_TRUE(coat && coat.HasComponent<GroomWetnessComponent>());
        EXPECT_TRUE(coat.GetComponent<GroomWetnessComponent>() == GroomWetnessComponent{});
        const auto wild = loaded->FindEntityByName("Wild");
        ASSERT_TRUE(wild && wild.HasComponent<GroomWetnessComponent>());
        const auto& w = wild.GetComponent<GroomWetnessComponent>();
        EXPECT_FLOAT_EQ(w.m_Band, 1.0f);
        EXPECT_FLOAT_EQ(w.m_Darkening, 1.0f);
        EXPECT_FLOAT_EQ(w.m_Immersion, 1.0f);
    }

    TEST(GroomWetnessSerialization, SaveGamePayloadRoundTripsAndConsumesExactlyItsFields)
    {
        GroomWetnessComponent authored = AuthoredWetness();
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        SaveGameComponentSerializer::Serialize(writer, authored);
        GroomWetnessComponent loaded;
        FMemoryReader reader(bytes);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd());
        EXPECT_TRUE(loaded == authored) << "a wet coat saved mid-walk loads exactly as wet";
    }

    TEST(GroomWetnessSerialization, SaveGameSanitizesCorruptFloats)
    {
        GroomWetnessComponent authored;
        authored.m_Waterline = kNaN;
        authored.m_Band = std::numeric_limits<f32>::infinity();
        authored.m_Darkening = kNaN;
        authored.m_Immersion = kNaN;
        authored.m_SoakedLine = -std::numeric_limits<f32>::infinity();
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        SaveGameComponentSerializer::Serialize(writer, authored);
        GroomWetnessComponent loaded;
        FMemoryReader reader(bytes);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        const GroomWetnessComponent defaults;
        EXPECT_FLOAT_EQ(loaded.m_Waterline, defaults.m_Waterline);
        EXPECT_FLOAT_EQ(loaded.m_Band, defaults.m_Band);
        EXPECT_FLOAT_EQ(loaded.m_Darkening, defaults.m_Darkening);
        EXPECT_FLOAT_EQ(loaded.m_Immersion, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_SoakedLine, defaults.m_SoakedLine);
    }

    TEST(GroomWetnessSerialization, BinarySidecarAndCookedSceneKeepTheCoatsWetness)
    {
        auto authored = Scene::Create();
        auto entity = authored->CreateEntity("Packed coat");
        entity.AddComponent<GroomWetnessComponent>() = AuthoredWetness();
        const auto path = TempDir() / "wet.olo";
        SceneSerializer(authored).Serialize(path);
        auto first = Scene::Create();
        ASSERT_TRUE(SceneSerializer(first).Deserialize(path));
        ASSERT_TRUE(std::filesystem::exists(path.string() + ".scenebin"));
        auto cached = Scene::Create();
        ASSERT_TRUE(SceneSerializer(cached).Deserialize(path));
        EXPECT_TRUE(cached->FindEntityByName("Packed coat").GetComponent<GroomWetnessComponent>() == AuthoredWetness());

        SceneAssetSerializer serializer;
        const auto yaml = serializer.SerializeToString(authored);
        const auto pack = TempDir() / "wet.olopack";
        AssetPackFile::SceneInfo info{};
        info.Handle = 1615;
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
        EXPECT_TRUE(loaded->FindEntityByName("Packed coat").GetComponent<GroomWetnessComponent>() == AuthoredWetness());
    }
} // namespace OloEngine::Tests
