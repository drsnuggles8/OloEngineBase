#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomSceneTemporalResolveTest — issue #1429.
//
// "An authored groom scene renders bald without TAA, and a scene cannot ask
//  for TAA."
//
// Every coat in Scenes/GroomAnimals.olo asks for StochasticAlpha, which
// SelectGroomComposition refuses without a temporal resolve, and the tier it
// falls back to draws no sub-pixel hair. The fix is that a scene holding such a
// groom ASKS for the resolve, every frame, rather than storing TAA in the file
// where it could go stale. This file pins the CPU half of that chain:
//
//   the policy     WantsEngineTAA: user setting OR an honoured scene request
//   the producer   Scene::CountGroomsNeedingTemporalResolve, which filter
//   the loader     a stochastic groom that came back through the scene YAML
//                  still asks, so "open the scene" is enough
//   the asset      the shipped GroomAnimals.olo holds grooms that will ask
//
// The pixel half (the coats are there with TAA unticked, and bald when the
// request is refused) is GroomAnimalsAcceptanceEvidenceTest's
// AFreshSceneWithTAAOffStillCoatsItsSubjects.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscalePolicy.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

using namespace OloEngine;

namespace
{
    constexpr AssetHandle kSomeGroom = 0x1429'0000'0000'0001ull;

    Entity AddGroom(Scene& scene, const std::string& name, GroomCompositionMode mode, bool renderStrands = true,
                    AssetHandle groom = kSomeGroom)
    {
        Entity entity = scene.CreateEntity(name);
        auto& gc = entity.AddComponent<GroomComponent>();
        gc.m_Groom = groom;
        gc.m_RenderStrands = renderStrands;
        gc.m_CompositionMode = static_cast<u8>(mode);
        return entity;
    }
} // namespace

// ── The policy ──────────────────────────────────────────────────────────────

TEST(GroomSceneTemporalResolve, ASceneRequestTurnsTAAOnOnlyWhenHonoured)
{
    using TemporalUpscalePolicy::WantsEngineTAA;
    // The user's own setting wins in every row: a request can only ADD a resolve.
    EXPECT_TRUE(WantsEngineTAA(true, false, false));
    EXPECT_TRUE(WantsEngineTAA(true, false, true));
    EXPECT_TRUE(WantsEngineTAA(true, true, false));
    EXPECT_TRUE(WantsEngineTAA(true, true, true));
    // The fix: TAA unticked, a stochastic groom in the scene, request honoured.
    EXPECT_TRUE(WantsEngineTAA(false, true, true));
    // The diagnostic switch reaches the refused tier again.
    EXPECT_FALSE(WantsEngineTAA(false, true, false));
    // No request, no TAA: a scene without a stochastic groom is untouched.
    EXPECT_FALSE(WantsEngineTAA(false, false, true));
    EXPECT_FALSE(WantsEngineTAA(false, false, false));
}

TEST(GroomSceneTemporalResolve, FSR2StillSubsumesARequestedResolve)
{
    // A requested resolve is still ENGINE TAA, and FSR2 is itself a resolve: the
    // two must not stack. The groom then gets its resolve from FSR2.
    const bool wanted = TemporalUpscalePolicy::WantsEngineTAA(false, true, true);
    EXPECT_FALSE(TemporalUpscalePolicy::ShouldRunEngineTAA(wanted, true));
    EXPECT_TRUE(TemporalUpscalePolicy::ShouldRunEngineTAA(wanted, false));
}

// ── The producer ────────────────────────────────────────────────────────────

TEST(GroomSceneTemporalResolve, OnlyDrawnStochasticGroomsAsk)
{
    auto scene = Scene::Create();
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 0u) << "an empty scene asks for nothing";

    AddGroom(*scene, "Stochastic", GroomCompositionMode::StochasticAlpha);
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 1u);

    // One knocked-out input per groom, each of which must NOT ask.
    AddGroom(*scene, "Opaque", GroomCompositionMode::OpaqueRibbon);
    AddGroom(*scene, "AlphaToCoverage", GroomCompositionMode::AlphaToCoverage);
    AddGroom(*scene, "OIT", GroomCompositionMode::WeightedBlendedOIT);
    AddGroom(*scene, "StrandsOff", GroomCompositionMode::StochasticAlpha, false);
    AddGroom(*scene, "NoAsset", GroomCompositionMode::StochasticAlpha, true, 0);
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 1u)
        << "only a groom that draws strands, has an asset and asks for StochasticAlpha needs a resolve";

    AddGroom(*scene, "Second", GroomCompositionMode::StochasticAlpha);
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 2u) << "a count, not a flag";
}

TEST(GroomSceneTemporalResolve, TheRequestCannotGoStale)
{
    // The whole argument for requesting over persisting TAA: remove the reason
    // and the request is gone on the next frame, with nothing left in a file.
    auto scene = Scene::Create();
    Entity groom = AddGroom(*scene, "Coat", GroomCompositionMode::StochasticAlpha);
    ASSERT_EQ(scene->CountGroomsNeedingTemporalResolve(), 1u);

    groom.GetComponent<GroomComponent>().m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 0u);

    groom.GetComponent<GroomComponent>().m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha);
    scene->DestroyEntity(groom);
    EXPECT_EQ(scene->CountGroomsNeedingTemporalResolve(), 0u);
}

// ── The loader ──────────────────────────────────────────────────────────────

TEST(GroomSceneTemporalResolve, AStochasticGroomLoadedFromSceneYamlAsks)
{
    std::string yaml;
    {
        auto authored = Scene::Create();
        AddGroom(*authored, "Coat", GroomCompositionMode::StochasticAlpha);
        AddGroom(*authored, "Baseline", GroomCompositionMode::OpaqueRibbon);
        yaml = SceneSerializer(authored).SerializeToYAML();
    }
    ASSERT_FALSE(yaml.empty());

    auto reloaded = Scene::Create();
    ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));
    EXPECT_EQ(reloaded->CountGroomsNeedingTemporalResolve(), 1u)
        << "opening a scene with a stochastic groom must be enough to get its resolve";
}

// ── The asset the issue was filed against ───────────────────────────────────

TEST(GroomSceneTemporalResolve, TheShippedGroomAnimalsSceneHoldsGroomsThatAsk)
{
    // Structural, not a full Deserialize (that needs a mounted project and the
    // cooked grooms; see AssetContentValidityTest). What matters is that the
    // file the editor opens holds the keys the producer above filters on, with
    // values that make it ask. If the scene were re-exported to OpaqueRibbon
    // this would say so, and the fix would silently stop being exercised live.
    const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Scenes/GroomAnimals.olo";
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
    const YAML::Node root = YAML::LoadFile(path.string());
    const YAML::Node entities = root["Entities"];
    ASSERT_TRUE(entities && entities.IsSequence());

    u32 asking = 0;
    u32 grooms = 0;
    for (const auto& entity : entities)
    {
        const YAML::Node gc = entity["GroomComponent"];
        if (!gc)
            continue;
        ++grooms;
        const bool renderStrands = gc["RenderStrands"] && gc["RenderStrands"].as<bool>(false);
        const bool hasGroom = gc["Groom"] && gc["Groom"].as<u64>(0) != 0;
        const bool stochastic = gc["CompositionMode"] &&
                                gc["CompositionMode"].as<i32>(-1) == static_cast<i32>(GroomCompositionMode::StochasticAlpha);
        if (renderStrands && hasGroom && stochastic)
            ++asking;
    }
    EXPECT_EQ(grooms, 3u) << "two horses and a human";
    EXPECT_EQ(asking, grooms) << "every coat in the scene is stochastic, so every one must request the resolve";
}
