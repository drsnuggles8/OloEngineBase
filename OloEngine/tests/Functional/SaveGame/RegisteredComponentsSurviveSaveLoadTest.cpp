#include "OloEnginePCH.h"

// =============================================================================
// RegisteredComponentsSurviveSaveLoadTest — Functional Test.
//
// Cross-subsystem seam under test:
//   28 component types × SaveGameSerializer capture/restore. These types
//   (gameplay progress, AI blackboards, animation pose state, Lua bindings,
//   rendering config) all have registered Serialize() overloads but were
//   missing from the SAVE_COMPONENT / TRY_LOAD_COMPONENT enumeration lists
//   (issue #325) — they round-tripped through scene YAML but silently
//   vanished through save-games. A regression looks like "loaded a quicksave
//   and the inventory / quest journal / IK pose / dialogue trigger was reset
//   to defaults" with no error logged.
//
// Scenario: build a scene with one entity per affected component, each
// carrying distinctly non-default values. Capture the scene state, restore
// into a fresh scene, and assert every component survived with its data.
//
// The companion meta-test (SaveGameComponentSerializerCoverageTest) pins the
// source-level list synchronisation; this test proves the wiring actually
// moves the bytes end-to-end for representative fields of every type.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // One scene entity per component type so a failure in one serializer
    // can't mask another, and the failure message names the exact component.
    constexpr const char* kEntityName = "Holder_";
} // namespace

class RegisteredComponentsSurviveSaveLoadTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto make = [this](const char* componentName) -> Entity
        {
            return GetScene().CreateEntity(std::string(kEntityName) + componentName);
        };

        // --- Animation ---
        make("SkeletonComponent").AddComponent<SkeletonComponent>();

        {
            auto& ik = make("IKTargetComponent").AddComponent<IKTargetComponent>();
            ik.AimIKEnabled = true;
            ik.AimBoneIndex = 5;
            ik.AimTarget = { 1.0f, 2.0f, 3.0f };
            ik.AimWeight = 0.8f;
            ik.AimTargetEntity = UUID{ 1111 };
            ik.LimbIKEnabled = true;
            ik.LimbBoneIndex = 9;
            ik.LimbTarget = { 4.0f, 5.0f, 6.0f };
            ik.LimbChainLength = 3;
            ik.LimbWeight = 0.6f;
            ik.LimbTargetEntity = UUID{ 2222 };
        }
        {
            auto& morph = make("MorphTargetComponent").AddComponent<MorphTargetComponent>();
            morph.Weights["smile"] = 0.7f;
            morph.Weights["frown"] = 0.1f;
        }
        {
            auto& graph = make("AnimationGraphComponent").AddComponent<AnimationGraphComponent>();
            graph.AnimationGraphAssetHandle = AssetHandle{ 4242 };
        }

        // --- Video / rendering config ---
        {
            auto& video = make("VideoOverlayComponent").AddComponent<VideoOverlayComponent>();
            video.VideoPath = "assets/video/intro.mpg";
            video.PlayOnStart = true;
            video.SkipOnInput = false;
            video.Looping = true;
            video.Volume = 0.25f;
        }
        {
            auto& surface = make("VideoSurfaceComponent").AddComponent<VideoSurfaceComponent>();
            surface.VideoPath = "assets/video/screen.mpg";
            surface.AutoPlay = false;
            surface.Looping = false;
            surface.Volume = 0.75f;
        }
        {
            auto& light = make("SphereAreaLightComponent").AddComponent<SphereAreaLightComponent>();
            light.m_Color = { 0.2f, 0.4f, 0.8f };
            light.m_Intensity = 3.5f;
            light.m_Radius = 2.0f;
            light.m_Range = 25.0f;
            light.m_CastShadows = true;
        }
        {
            // Values inside the load-time sanitisation clamps (Turbidity
            // 1.7–10, Exposure 0–10, SunIntensity 0–100, resolution 8–4096)
            // so the assertions below see them verbatim.
            auto& sky = make("ProceduralSkyComponent").AddComponent<ProceduralSkyComponent>();
            sky.m_SunDirection = { 0.2f, 0.8f, 0.3f };
            sky.m_Turbidity = 4.0f;
            sky.m_Exposure = 0.5f;
            sky.m_SunIntensity = 2.5f;
            sky.m_EnableSkybox = true;
            sky.m_EnableIBL = true;
            sky.m_IBLIntensity = 2.0f;
            sky.m_CubemapResolution = 512u;
        }
        {
            auto& stars = make("StarNestSkyComponent").AddComponent<StarNestSkyComponent>();
            stars.m_EnableSkybox = true;
            stars.m_EnableIBL = true;
            stars.m_IBLIntensity = 2.0f;
            stars.m_CubemapResolution = 512u;
        }
        {
            auto& tiles = make("TileRendererComponent").AddComponent<TileRendererComponent>();
            tiles.ResizeGrid(4, 3);
            tiles.TileSize = 2.5f;
            tiles.MaterialIDs[5] = 1;
            tiles.Materials.emplace_back();
            tiles.Materials[1].SetBaseColorFactor({ 1.0f, 0.0f, 0.0f, 1.0f });
            tiles.Materials[1].SetMetallicFactor(0.25f);
            tiles.Materials[1].SetRoughnessFactor(0.5f);
        }
        {
            auto& instanced = make("InstancedMeshComponent").AddComponent<InstancedMeshComponent>();
            instanced.Primitive = MeshPrimitive::Cube;
            instanced.PlacementAssetHandle = AssetHandle{ 99 };
            instanced.FrustumCullPerInstance = false;
            instanced.CastShadows = false;
            instanced.CullDistance = 42.0f;
            instanced.Instances.resize(2);
            instanced.Instances[0].Transform = glm::translate(glm::mat4(1.0f), { 1.0f, 2.0f, 3.0f });
            instanced.Instances[1].Transform = glm::translate(glm::mat4(1.0f), { 4.0f, 5.0f, 6.0f });
        }

        // --- Physics ---
        {
            auto& buoyancy = make("BuoyancyComponent").AddComponent<BuoyancyComponent>();
            buoyancy.m_Enabled = false;
            buoyancy.m_ProbeExtents = { 1.0f, 2.0f, 3.0f };
            buoyancy.m_FluidDensity = 500.0f;
            buoyancy.m_BuoyancyScale = 2.0f;
            buoyancy.m_LinearDrag = 1.5f;
            buoyancy.m_AngularDrag = 0.75f;
            buoyancy.m_SubmergenceRamp = 0.5f;
        }

        // --- Scripting / dialogue ---
        {
            auto& lua = make("LuaScriptComponent").AddComponent<LuaScriptComponent>();
            lua.ScriptFile = "assets/scripts/turret.lua";
        }
        {
            auto& dialogue = make("DialogueComponent").AddComponent<DialogueComponent>();
            dialogue.m_DialogueTree = AssetHandle{ 123456 };
            dialogue.m_AutoTrigger = true;
            dialogue.m_TriggerRadius = 7.5f;
            dialogue.m_TriggerOnce = false;
        }

        // --- Navigation ---
        {
            auto& bounds = make("NavMeshBoundsComponent").AddComponent<NavMeshBoundsComponent>();
            bounds.m_Min = { -5.0f, -1.0f, -5.0f };
            bounds.m_Max = { 5.0f, 9.0f, 5.0f };
            bounds.m_Links.emplace_back(glm::vec3{ -3.0f, 0.5f, 1.0f }, glm::vec3{ 3.0f, 0.5f, -1.0f },
                                        /*radius=*/0.75f, /*bidirectional=*/false);
        }
        {
            auto& agent = make("NavAgentComponent").AddComponent<NavAgentComponent>();
            agent.m_Radius = 0.9f;
            agent.m_Height = 1.6f;
            agent.m_MaxSpeed = 6.0f;
            agent.m_Acceleration = 12.0f;
            agent.m_StoppingDistance = 0.25f;
            agent.m_AvoidancePriority = 7;
            agent.m_LockYAxis = true;
        }

        // --- UI / world ---
        {
            auto& nameplate = make("NameplateComponent").AddComponent<NameplateComponent>();
            nameplate.m_ShowManaBar = true;
            nameplate.m_WorldOffset = { 0.0f, 3.0f, 0.0f };
            nameplate.m_ManaBarGap = 0.05f;
        }
        {
            auto& anchor = make("UIWorldAnchorComponent").AddComponent<UIWorldAnchorComponent>();
            anchor.m_TargetEntity = UUID{ 777 };
            anchor.m_WorldOffset = { 0.0f, 4.0f, 0.0f };
        }

        // --- Cinematic ---
        {
            auto& cine = make("CinematicComponent").AddComponent<CinematicComponent>();
            cine.Sequence = AssetHandle{ 55 };
            cine.PlayOnStart = true;
            cine.Loop = true;
            cine.PlaybackSpeed = 2.0f;
        }

        // --- AI (blackboards are the save-game-specific payload: they hold
        //     in-progress decision state that scene YAML never carries) ---
        {
            auto& bt = make("BehaviorTreeComponent").AddComponent<BehaviorTreeComponent>();
            bt.BehaviorTreeAssetHandle = AssetHandle{ 77 };
            bt.Blackboard.Set("alerted", true);
            bt.Blackboard.Set("targetDist", 12.5f);
            bt.Blackboard.Set("home", glm::vec3{ 1.0f, 2.0f, 3.0f });
        }
        {
            auto& fsm = make("StateMachineComponent").AddComponent<StateMachineComponent>();
            fsm.StateMachineAssetHandle = AssetHandle{ 88 };
            fsm.Blackboard.Set("state", std::string{ "patrol" });
        }
        {
            auto& goap = make("GoapAgentComponent").AddComponent<GoapAgentComponent>();
            goap.Enabled = false;
            goap.Blackboard.Set("hunger", 42);
        }

        // --- Gameplay: items / quests / abilities ---
        // Inventory::AddItemToSlot validates ItemDefinitionID against the
        // process-wide ItemDatabase — on BOTH the build side here and the
        // save-game restore side (SerializeInventory's load loop), so the
        // definitions must exist or items are dropped.
        ItemDatabase::Clear();
        {
            ItemDefinition sword;
            sword.ItemID = "sword_iron";
            sword.DisplayName = "Iron Sword";
            sword.MaxStackSize = 1;
            ItemDatabase::Register(sword);

            ItemDefinition potion;
            potion.ItemID = "potion_health";
            potion.DisplayName = "Health Potion";
            potion.MaxStackSize = 20;
            ItemDatabase::Register(potion);
        }
        {
            auto& inv = make("InventoryComponent").AddComponent<InventoryComponent>();
            ItemInstance sword;
            sword.ItemDefinitionID = "sword_iron";
            sword.StackCount = 1;
            sword.Durability = 80.0f;
            sword.MaxDurability = 100.0f;
            ASSERT_TRUE(inv.PlayerInventory.AddItemToSlot(3, sword));
            inv.Currency = 1234;
        }
        {
            auto& pickup = make("ItemPickupComponent").AddComponent<ItemPickupComponent>();
            pickup.Item.ItemDefinitionID = "potion_health";
            pickup.Item.StackCount = 5;
            pickup.PickupRadius = 4.0f;
            pickup.AutoPickup = true;
            pickup.DespawnTimer = 30.0f;
        }
        {
            auto& container = make("ItemContainerComponent").AddComponent<ItemContainerComponent>();
            container.IsShop = true;
            container.LootTableID = "chest_dungeon_01";
            container.HasBeenLooted = true;
        }
        {
            auto& journal = make("QuestJournalComponent").AddComponent<QuestJournalComponent>();
            journal.Journal.SetPlayerLevel(7);
            journal.Journal.AddCompletedQuestID("q_intro", "branchA");
            journal.Journal.AddTag("met_king");
            journal.Journal.SetReputation("guild", 25);
        }
        {
            auto& giver = make("QuestGiverComponent").AddComponent<QuestGiverComponent>();
            giver.OfferedQuestIDs = { "q_fetch", "q_slay" };
            giver.TurnInQuestIDs = { "q_intro" };
            giver.QuestMarkerIcon = "!";
        }
        {
            auto& abilities = make("AbilityComponent").AddComponent<AbilityComponent>();
            abilities.Attributes.DefineAttribute("Health", 73.5f);
            abilities.OwnedTags.AddTag(GameplayTag{ "State.Alive" });
            GameplayAbilityDef def;
            def.Name = "Heal";
            def.CooldownDuration = 2.5f;
            ActiveAbility ability;
            ability.Definition = std::move(def);
            abilities.Abilities.push_back(std::move(ability));
        }
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        ItemDatabase::Clear(); // don't leak the test definitions into other tests
    }

    // Find the restored holder entity for a component type and hand its
    // component to `check`. Reports (rather than aborts) when the component
    // was dropped, so every type gets a verdict in one run.
    template<typename T, typename CheckFn>
    static void Verify(Scene& restored, const char* componentName, CheckFn&& check)
    {
        Entity entity = restored.FindEntityByName(std::string(kEntityName) + componentName);
        if (!entity)
        {
            ADD_FAILURE() << componentName << ": holder entity missing from restored scene";
            return;
        }
        if (!entity.HasComponent<T>())
        {
            ADD_FAILURE() << componentName
                          << " dropped by the save-game round-trip — check the "
                             "SAVE_COMPONENT/TRY_LOAD_COMPONENT lists in SaveGameSerializer.cpp";
            return;
        }
        check(entity.GetComponent<T>());
    }
};

TEST_F(RegisteredComponentsSurviveSaveLoadTest, PreviouslyDroppedComponentsRoundTripThroughSaveGame)
{
    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u) << "CaptureSceneState failed with the new components present.";

    Ref<Scene> restoredScene = Scene::Create();
    restoredScene->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restoredScene, payload));
    Scene& restored = *restoredScene;

    Verify<SkeletonComponent>(restored, "SkeletonComponent", [](const SkeletonComponent&) { /* marker-only serializer: presence is the contract */ });

    Verify<IKTargetComponent>(restored, "IKTargetComponent",
                              [](const IKTargetComponent& ik)
                              {
                                  EXPECT_TRUE(ik.AimIKEnabled);
                                  EXPECT_EQ(ik.AimBoneIndex, 5u);
                                  EXPECT_FLOAT_EQ(ik.AimTarget.z, 3.0f);
                                  EXPECT_FLOAT_EQ(ik.AimWeight, 0.8f);
                                  EXPECT_EQ(static_cast<u64>(ik.AimTargetEntity), 1111u);
                                  EXPECT_TRUE(ik.LimbIKEnabled);
                                  EXPECT_EQ(ik.LimbBoneIndex, 9u);
                                  EXPECT_EQ(ik.LimbChainLength, 3u);
                                  EXPECT_FLOAT_EQ(ik.LimbWeight, 0.6f);
                                  EXPECT_EQ(static_cast<u64>(ik.LimbTargetEntity), 2222u);
                              });

    Verify<MorphTargetComponent>(restored, "MorphTargetComponent",
                                 [](const MorphTargetComponent& morph)
                                 {
                                     ASSERT_TRUE(morph.Weights.contains("smile"));
                                     ASSERT_TRUE(morph.Weights.contains("frown"));
                                     EXPECT_FLOAT_EQ(morph.Weights.at("smile"), 0.7f);
                                     EXPECT_FLOAT_EQ(morph.Weights.at("frown"), 0.1f);
                                 });

    Verify<AnimationGraphComponent>(restored, "AnimationGraphComponent",
                                    [](const AnimationGraphComponent& graph)
                                    {
                                        EXPECT_EQ(static_cast<u64>(graph.AnimationGraphAssetHandle), 4242u);
                                    });

    Verify<VideoOverlayComponent>(restored, "VideoOverlayComponent",
                                  [](const VideoOverlayComponent& video)
                                  {
                                      EXPECT_EQ(video.VideoPath, "assets/video/intro.mpg");
                                      EXPECT_TRUE(video.PlayOnStart);
                                      EXPECT_FALSE(video.SkipOnInput);
                                      EXPECT_TRUE(video.Looping);
                                      EXPECT_FLOAT_EQ(video.Volume, 0.25f);
                                  });

    Verify<VideoSurfaceComponent>(restored, "VideoSurfaceComponent",
                                  [](const VideoSurfaceComponent& surface)
                                  {
                                      EXPECT_EQ(surface.VideoPath, "assets/video/screen.mpg");
                                      EXPECT_FALSE(surface.AutoPlay);
                                      EXPECT_FALSE(surface.Looping);
                                      EXPECT_FLOAT_EQ(surface.Volume, 0.75f);
                                  });

    Verify<SphereAreaLightComponent>(restored, "SphereAreaLightComponent",
                                     [](const SphereAreaLightComponent& light)
                                     {
                                         EXPECT_FLOAT_EQ(light.m_Color.b, 0.8f);
                                         EXPECT_FLOAT_EQ(light.m_Intensity, 3.5f);
                                         EXPECT_FLOAT_EQ(light.m_Radius, 2.0f);
                                         EXPECT_FLOAT_EQ(light.m_Range, 25.0f);
                                         EXPECT_TRUE(light.m_CastShadows);
                                     });

    Verify<ProceduralSkyComponent>(restored, "ProceduralSkyComponent",
                                   [](const ProceduralSkyComponent& sky)
                                   {
                                       EXPECT_FLOAT_EQ(sky.m_SunDirection.y, 0.8f);
                                       EXPECT_FLOAT_EQ(sky.m_Turbidity, 4.0f);
                                       EXPECT_FLOAT_EQ(sky.m_Exposure, 0.5f);
                                       EXPECT_FLOAT_EQ(sky.m_SunIntensity, 2.5f);
                                       EXPECT_TRUE(sky.m_EnableSkybox);
                                       EXPECT_TRUE(sky.m_EnableIBL);
                                       EXPECT_FLOAT_EQ(sky.m_IBLIntensity, 2.0f);
                                       EXPECT_EQ(sky.m_CubemapResolution, 512u);
                                   });

    Verify<StarNestSkyComponent>(restored, "StarNestSkyComponent",
                                 [](const StarNestSkyComponent& stars)
                                 {
                                     EXPECT_TRUE(stars.m_EnableSkybox);
                                     EXPECT_TRUE(stars.m_EnableIBL);
                                     EXPECT_FLOAT_EQ(stars.m_IBLIntensity, 2.0f);
                                     EXPECT_EQ(stars.m_CubemapResolution, 512u);
                                 });

    Verify<TileRendererComponent>(restored, "TileRendererComponent",
                                  [](const TileRendererComponent& tiles)
                                  {
                                      EXPECT_EQ(tiles.Width, 4u);
                                      EXPECT_EQ(tiles.Height, 3u);
                                      EXPECT_FLOAT_EQ(tiles.TileSize, 2.5f);
                                      ASSERT_EQ(tiles.MaterialIDs.size(), 12u);
                                      EXPECT_EQ(tiles.MaterialIDs[5], 1);
                                      ASSERT_EQ(tiles.Materials.size(), 2u);
                                      EXPECT_FLOAT_EQ(tiles.Materials[1].GetBaseColorFactor().r, 1.0f);
                                      EXPECT_FLOAT_EQ(tiles.Materials[1].GetMetallicFactor(), 0.25f);
                                      EXPECT_FLOAT_EQ(tiles.Materials[1].GetRoughnessFactor(), 0.5f);
                                  });

    Verify<InstancedMeshComponent>(restored, "InstancedMeshComponent",
                                   [](const InstancedMeshComponent& instanced)
                                   {
                                       EXPECT_EQ(instanced.Primitive, MeshPrimitive::Cube);
                                       EXPECT_EQ(static_cast<u64>(instanced.PlacementAssetHandle), 99u);
                                       EXPECT_FALSE(instanced.FrustumCullPerInstance);
                                       EXPECT_FALSE(instanced.CastShadows);
                                       EXPECT_FLOAT_EQ(instanced.CullDistance, 42.0f);
                                       ASSERT_EQ(instanced.Instances.size(), 2u);
                                       EXPECT_FLOAT_EQ(instanced.Instances[0].Transform[3][2], 3.0f);
                                       EXPECT_FLOAT_EQ(instanced.Instances[1].Transform[3][0], 4.0f);
                                   });

    Verify<BuoyancyComponent>(restored, "BuoyancyComponent",
                              [](const BuoyancyComponent& buoyancy)
                              {
                                  EXPECT_FALSE(buoyancy.m_Enabled);
                                  EXPECT_FLOAT_EQ(buoyancy.m_ProbeExtents.z, 3.0f);
                                  EXPECT_FLOAT_EQ(buoyancy.m_FluidDensity, 500.0f);
                                  EXPECT_FLOAT_EQ(buoyancy.m_BuoyancyScale, 2.0f);
                                  EXPECT_FLOAT_EQ(buoyancy.m_LinearDrag, 1.5f);
                                  EXPECT_FLOAT_EQ(buoyancy.m_AngularDrag, 0.75f);
                                  EXPECT_FLOAT_EQ(buoyancy.m_SubmergenceRamp, 0.5f);
                              });

    Verify<LuaScriptComponent>(restored, "LuaScriptComponent",
                               [](const LuaScriptComponent& lua)
                               {
                                   EXPECT_EQ(lua.ScriptFile, "assets/scripts/turret.lua");
                               });

    Verify<DialogueComponent>(restored, "DialogueComponent",
                              [](const DialogueComponent& dialogue)
                              {
                                  EXPECT_EQ(static_cast<u64>(dialogue.m_DialogueTree), 123456u);
                                  EXPECT_TRUE(dialogue.m_AutoTrigger);
                                  EXPECT_FLOAT_EQ(dialogue.m_TriggerRadius, 7.5f);
                                  EXPECT_FALSE(dialogue.m_TriggerOnce);
                              });

    Verify<NavMeshBoundsComponent>(restored, "NavMeshBoundsComponent",
                                   [](const NavMeshBoundsComponent& bounds)
                                   {
                                       EXPECT_FLOAT_EQ(bounds.m_Min.x, -5.0f);
                                       EXPECT_FLOAT_EQ(bounds.m_Max.y, 9.0f);
                                       ASSERT_EQ(bounds.m_Links.size(), 1u);
                                       const auto& link = bounds.m_Links.front();
                                       EXPECT_FLOAT_EQ(link.m_Start.x, -3.0f);
                                       EXPECT_FLOAT_EQ(link.m_End.z, -1.0f);
                                       EXPECT_FLOAT_EQ(link.m_Radius, 0.75f);
                                       EXPECT_FALSE(link.m_Bidirectional);
                                   });

    Verify<NavAgentComponent>(restored, "NavAgentComponent",
                              [](const NavAgentComponent& agent)
                              {
                                  EXPECT_FLOAT_EQ(agent.m_Radius, 0.9f);
                                  EXPECT_FLOAT_EQ(agent.m_Height, 1.6f);
                                  EXPECT_FLOAT_EQ(agent.m_MaxSpeed, 6.0f);
                                  EXPECT_FLOAT_EQ(agent.m_Acceleration, 12.0f);
                                  EXPECT_FLOAT_EQ(agent.m_StoppingDistance, 0.25f);
                                  EXPECT_EQ(agent.m_AvoidancePriority, 7);
                                  EXPECT_TRUE(agent.m_LockYAxis);
                              });

    Verify<NameplateComponent>(restored, "NameplateComponent",
                               [](const NameplateComponent& nameplate)
                               {
                                   EXPECT_TRUE(nameplate.m_ShowManaBar);
                                   EXPECT_FLOAT_EQ(nameplate.m_WorldOffset.y, 3.0f);
                                   EXPECT_FLOAT_EQ(nameplate.m_ManaBarGap, 0.05f);
                               });

    Verify<UIWorldAnchorComponent>(restored, "UIWorldAnchorComponent",
                                   [](const UIWorldAnchorComponent& anchor)
                                   {
                                       EXPECT_EQ(static_cast<u64>(anchor.m_TargetEntity), 777u);
                                       EXPECT_FLOAT_EQ(anchor.m_WorldOffset.y, 4.0f);
                                   });

    Verify<CinematicComponent>(restored, "CinematicComponent",
                               [](const CinematicComponent& cine)
                               {
                                   EXPECT_EQ(static_cast<u64>(cine.Sequence), 55u);
                                   EXPECT_TRUE(cine.PlayOnStart);
                                   EXPECT_TRUE(cine.Loop);
                                   EXPECT_FLOAT_EQ(cine.PlaybackSpeed, 2.0f);
                               });

    Verify<BehaviorTreeComponent>(restored, "BehaviorTreeComponent",
                                  [](const BehaviorTreeComponent& bt)
                                  {
                                      EXPECT_EQ(static_cast<u64>(bt.BehaviorTreeAssetHandle), 77u);
                                      const auto& bb = bt.Blackboard.GetAll();
                                      ASSERT_TRUE(bb.contains("alerted"));
                                      ASSERT_TRUE(bb.contains("targetDist"));
                                      ASSERT_TRUE(bb.contains("home"));
                                      EXPECT_TRUE(std::get<bool>(bb.at("alerted")));
                                      EXPECT_FLOAT_EQ(std::get<f32>(bb.at("targetDist")), 12.5f);
                                      EXPECT_FLOAT_EQ(std::get<glm::vec3>(bb.at("home")).z, 3.0f);
                                  });

    Verify<StateMachineComponent>(restored, "StateMachineComponent",
                                  [](const StateMachineComponent& fsm)
                                  {
                                      EXPECT_EQ(static_cast<u64>(fsm.StateMachineAssetHandle), 88u);
                                      const auto& bb = fsm.Blackboard.GetAll();
                                      ASSERT_TRUE(bb.contains("state"));
                                      EXPECT_EQ(std::get<std::string>(bb.at("state")), "patrol");
                                  });

    Verify<GoapAgentComponent>(restored, "GoapAgentComponent",
                               [](const GoapAgentComponent& goap)
                               {
                                   EXPECT_FALSE(goap.Enabled);
                                   const auto& bb = goap.Blackboard.GetAll();
                                   ASSERT_TRUE(bb.contains("hunger"));
                                   EXPECT_EQ(std::get<i32>(bb.at("hunger")), 42);
                               });

    Verify<InventoryComponent>(restored, "InventoryComponent",
                               [](const InventoryComponent& inv)
                               {
                                   EXPECT_EQ(inv.Currency, 1234);
                                   const auto& slots = inv.PlayerInventory.GetSlots();
                                   ASSERT_GT(slots.size(), 3u);
                                   ASSERT_TRUE(slots[3].has_value())
                                       << "inventory slot 3 lost its item";
                                   EXPECT_EQ(slots[3]->ItemDefinitionID, "sword_iron");
                                   EXPECT_EQ(slots[3]->StackCount, 1);
                                   EXPECT_FLOAT_EQ(slots[3]->Durability, 80.0f);
                                   EXPECT_FLOAT_EQ(slots[3]->MaxDurability, 100.0f);
                               });

    Verify<ItemPickupComponent>(restored, "ItemPickupComponent",
                                [](const ItemPickupComponent& pickup)
                                {
                                    EXPECT_EQ(pickup.Item.ItemDefinitionID, "potion_health");
                                    EXPECT_EQ(pickup.Item.StackCount, 5);
                                    EXPECT_FLOAT_EQ(pickup.PickupRadius, 4.0f);
                                    EXPECT_TRUE(pickup.AutoPickup);
                                    EXPECT_FLOAT_EQ(pickup.DespawnTimer, 30.0f);
                                });

    Verify<ItemContainerComponent>(restored, "ItemContainerComponent",
                                   [](const ItemContainerComponent& container)
                                   {
                                       EXPECT_TRUE(container.IsShop);
                                       EXPECT_EQ(container.LootTableID, "chest_dungeon_01");
                                       EXPECT_TRUE(container.HasBeenLooted);
                                   });

    Verify<QuestJournalComponent>(restored, "QuestJournalComponent",
                                  [](const QuestJournalComponent& journal)
                                  {
                                      EXPECT_EQ(journal.Journal.GetPlayerLevel(), 7);
                                      EXPECT_TRUE(journal.Journal.GetCompletedQuestIDs().contains("q_intro"))
                                          << "completed-quest set lost q_intro";
                                      EXPECT_TRUE(journal.Journal.GetTags().contains("met_king"));
                                      const auto reputations = journal.Journal.GetReputations();
                                      ASSERT_TRUE(reputations.contains("guild"));
                                      EXPECT_EQ(reputations.at("guild"), 25);
                                  });

    Verify<QuestGiverComponent>(restored, "QuestGiverComponent",
                                [](const QuestGiverComponent& giver)
                                {
                                    EXPECT_EQ(giver.OfferedQuestIDs,
                                              (std::vector<std::string>{ "q_fetch", "q_slay" }));
                                    EXPECT_EQ(giver.TurnInQuestIDs, (std::vector<std::string>{ "q_intro" }));
                                    EXPECT_EQ(giver.QuestMarkerIcon, "!");
                                });

    Verify<AbilityComponent>(restored, "AbilityComponent",
                             [](const AbilityComponent& abilities)
                             {
                                 ASSERT_TRUE(abilities.Attributes.HasAttribute("Health"));
                                 EXPECT_NEAR(abilities.Attributes.GetBaseValue("Health"), 73.5f, 1e-4f);
                                 EXPECT_TRUE(abilities.OwnedTags.HasTagExact(GameplayTag{ "State.Alive" }));
                                 ASSERT_EQ(abilities.Abilities.size(), 1u);
                                 EXPECT_EQ(abilities.Abilities.front().Definition.Name, "Heal");
                                 EXPECT_NEAR(abilities.Abilities.front().Definition.CooldownDuration, 2.5f, 1e-4f);
                             });
}
