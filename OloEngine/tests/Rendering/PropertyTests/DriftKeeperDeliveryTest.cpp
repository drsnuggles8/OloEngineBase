// OLO_TEST_LAYER: L8
// Check the keeper as authored in Drift and as a shipping runtime loads him.
// Each lower level must deliver a skinned, expressive body with the same
// material slots, or a boat can approach a frozen or unshaded figure.
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;
        const fs::path kModelDir = "Assets/Models/DriftKeeper";
        const fs::path kScenePath = "Assets/Scenes/Drift.olo";
        const fs::path kSandbox = fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject";
        constexpr const char* kKeeper = "Stacks Light Keeper";
        constexpr const char* kTower = "Stacks Light";

        YAML::Node FindTag(const YAML::Node& scene, const char* tag)
        {
            for (const auto& entity : scene["Entities"])
                if (entity["TagComponent"] && entity["TagComponent"]["Tag"].as<std::string>() == tag)
                    return entity;
            return YAML::Node(YAML::NodeType::Undefined);
        }

        // Include the keeper's subtree and the tower transform it inherits.
        // Packing the unrelated island terrain and water would dwarf this case.
        YAML::Node KeeperSlice(const YAML::Node& authored)
        {
            YAML::Node slice;
            slice["Scene"] = authored["Scene"];
            slice["Version"] = authored["Version"];
            const auto tower = FindTag(authored, kTower);
            YAML::Node anchor;
            for (const char* field : { "Entity", "TagComponent", "TransformComponent", "RelationshipComponent" })
                if (tower[field])
                    anchor[field] = tower[field];
            slice["Entities"].push_back(anchor);
            std::set<u64> ids{ FindTag(authored, kKeeper)["Entity"].as<u64>() };
            sizet previous = 0;
            while (ids.size() != previous)
            {
                previous = ids.size();
                for (const auto& entity : authored["Entities"])
                    if (auto rel = entity["RelationshipComponent"]; rel && ids.contains(rel["ParentHandle"].as<u64>(0)))
                        ids.insert(entity["Entity"].as<u64>());
            }
            for (const auto& entity : authored["Entities"])
                if (ids.contains(entity["Entity"].as<u64>()))
                    slice["Entities"].push_back(entity);
            slice["Entities"].push_back(FindTag(authored, "Keeper Lamplight"));
            // Drive the actual boat transform and Drift clock from the test.
            // Sailing physics and the weather director run in the full game.
            for (const char* tag : { "Boat", "Atmosphere" })
            {
                const auto authoredInput = FindTag(authored, tag);
                YAML::Node input;
                for (const char* field : { "Entity", "TagComponent", "TransformComponent", "TimeOfDayComponent" })
                    if (authoredInput[field])
                        input[field] = authoredInput[field];
                slice["Entities"].push_back(input);
            }
            return slice;
        }

        TEST(DriftKeeperContract, TheSceneUsesTheGeneratedRig)
        {
            const Json rig = Json::parse(std::ifstream(kSandbox / kModelDir / "Keeper.rig.json"));
            const auto scene = YAML::LoadFile((kSandbox / kScenePath).string());
            const auto keeper = FindTag(scene, kKeeper);
            const auto tower = FindTag(scene, kTower);
            ASSERT_TRUE(keeper && tower);
            EXPECT_EQ(keeper["RelationshipComponent"]["ParentHandle"].as<u64>(), tower["Entity"].as<u64>());
            const auto transform = keeper["TransformComponent"];
            const auto& placement = rig.at("placement");
            for (int axis = 0; axis < 3; ++axis)
                EXPECT_NEAR(transform["Translation"][axis].as<f32>(), placement.at("translation").at(axis).get<f32>(), 1e-4f);
            EXPECT_NEAR(transform["Rotation"][1].as<f32>(), placement.at("yawRadians").get<f32>(), 1e-4f);
            EXPECT_EQ(fs::path(keeper["LuaScriptComponent"]["ScriptFile"].as<std::string>()).filename(), "DriftKeeper.lua");
            EXPECT_EQ(keeper["AnimationStateComponent"]["SourceFilePath"].as<std::string>(), (kModelDir / "Keeper.gltf").generic_string());

            ASSERT_EQ(rig.at("eyes").size(), 2u);
            for (const auto& eye : rig.at("eyes"))
            {
                const auto bone = eye.at("bone").get<std::string>();
                ASSERT_TRUE(bone == "eye_L" || bone == "eye_R");
                const auto entity = FindTag(scene, bone == "eye_L" ? "Keeper Eye L" : "Keeper Eye R");
                ASSERT_TRUE(entity);
                EXPECT_EQ(entity["BoneAttachmentComponent"]["BoneName"].as<std::string>(), bone);
                EXPECT_EQ(entity["RelationshipComponent"]["ParentHandle"].as<u64>(), keeper["Entity"].as<u64>());
                for (int axis = 0; axis < 3; ++axis)
                    EXPECT_NEAR(entity["TransformComponent"]["Scale"][axis].as<f32>(), rig.at("eyeRadius").get<f32>(), 1e-6f);
            }
            const auto groom = FindTag(scene, "Keeper Groom");
            ASSERT_TRUE(groom);
            const auto foundation = FindTag(scene, "Keeper Hair Foundation");
            ASSERT_TRUE(foundation);
            EXPECT_EQ(foundation["RelationshipComponent"]["ParentHandle"].as<u64>(), keeper["Entity"].as<u64>());
            EXPECT_EQ(foundation["BoneAttachmentComponent"]["BoneName"].as<std::string>(), "head");
            EXPECT_EQ(groom["RelationshipComponent"]["ParentHandle"].as<u64>(), foundation["Entity"].as<u64>());
            EXPECT_EQ(groom["GroomBindingComponent"]["TargetEntity"].as<u64>(), foundation["Entity"].as<u64>());

            const auto& morphs = rig.at("morphs");
            const auto& clips = rig.at("clips");
            for (const char* name : { "BlinkL", "BlinkR", "JawOpen", "Smile", "BrowRaise", "Squint", "AA", "EE", "OO" })
                EXPECT_NE(std::find(morphs.begin(), morphs.end(), name), morphs.end()) << name;
            for (const char* name : { "Idle", "Wave", "LanternRaise" })
                EXPECT_NE(std::find(clips.begin(), clips.end(), name), clips.end()) << name;
        }

        void CheckLevel(const Ref<MeshSource>& surface, const Json& gltf, const Json& morphs)
        {
            ASSERT_TRUE(surface);
            ASSERT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(surface));
            EXPECT_EQ(surface->GetBoneInfluences().Num(), surface->GetVertices().Num());
            const auto morphSet = surface->GetMorphTargets();
            ASSERT_TRUE(morphSet);
            ASSERT_EQ(morphSet->GetTargetCount(), morphs.size());
            for (const auto& target : morphSet->Targets)
            {
                const auto movesVertex = [](const MorphTargetVertex& delta)
                {
                    return glm::dot(delta.DeltaPosition, delta.DeltaPosition) > 1e-12f;
                };
                EXPECT_TRUE(target.IsSparse ? std::ranges::any_of(target.SparseVertices, movesVertex, &MorphTarget::SparseEntry::Delta)
                                            : std::ranges::any_of(target.Vertices, movesVertex))
                    << target.Name << " has no position deltas";
            }
            const auto& mesh = gltf.at("meshes").at(0);
            EXPECT_EQ(mesh.at("extras").at("targetNames"), morphs);
            const auto& primitives = mesh.at("primitives");
            std::set<std::string> actualMaterials;
            for (i32 i = 0; i < surface->GetSubmeshes().Num(); ++i)
            {
                const auto mat = surface->GetImportedMaterialForSubmesh(i);
                ASSERT_TRUE(mat);
                const std::string name(mat->GetName().ToView());
                actualMaterials.insert(name);
                const auto& materials = gltf.at("materials");
                const auto authored = std::ranges::find_if(materials, [&](const Json& item)
                                                           { return item.at("name").get<std::string>() == name; });
                ASSERT_NE(authored, materials.end()) << name;
                if (authored->contains("normalTexture"))
                {
                    // The cloth, skin and leather have paired 1024-square maps
                    // on every level. A 1x1 placeholder has the right type.
                    for (const auto& map : { mat->GetAlbedoMap(), mat->GetNormalMap() })
                    {
                        ASSERT_TRUE(map && map->IsLoaded()) << name;
                        EXPECT_EQ(map->GetWidth(), 1024u) << name;
                        EXPECT_EQ(map->GetHeight(), 1024u) << name;
                    }
                }
            }
            std::set<std::string> expectedMaterials;
            u32 indexCount = 0;
            for (const auto& primitive : primitives)
            {
                expectedMaterials.insert(gltf.at("materials").at(primitive.at("material").get<sizet>()).at("name").get<std::string>());
                indexCount += gltf.at("accessors").at(primitive.at("indices").get<sizet>()).at("count").get<u32>();
            }
            EXPECT_EQ(actualMaterials, expectedMaterials);
            EXPECT_EQ(static_cast<u32>(surface->GetIndices().Num()), indexCount);
        }

        void CheckGreetingInPlay(Scene& scene, Entity keeper)
        {
            Entity boat = scene.FindEntityByName("Boat");
            Entity sky = scene.FindEntityByName("Atmosphere");
            Entity lamp = scene.FindEntityByName("Keeper Lamplight");
            Entity wick = scene.FindEntityByName("Keeper Lamp Socket");
            Entity left = scene.FindEntityByName("Keeper Eye L");
            Entity right = scene.FindEntityByName("Keeper Eye R");
            Entity foundation = scene.FindEntityByName("Keeper Hair Foundation");
            ASSERT_TRUE(boat && sky && lamp && wick && left && right && foundation);
            ASSERT_TRUE(sky.HasComponent<TimeOfDayComponent>() && lamp.HasComponent<PointLightComponent>());
            ASSERT_TRUE(keeper.HasComponent<LuaScriptComponent>() && keeper.HasComponent<IKTargetComponent>());
            ASSERT_TRUE(foundation.HasComponent<MorphTargetComponent>() && keeper.HasComponent<MorphTargetComponent>());
            const auto script = Project::GetAssetFileSystemPath(keeper.GetComponent<LuaScriptComponent>().ScriptFile);
            ASSERT_TRUE(fs::is_regular_file(script)) << script.string();

            scene.SetRenderingEnabled(false);
            auto& clock = sky.GetComponent<TimeOfDayComponent>();
            clock.m_Paused = true;
            clock.m_TimeOfDayHours = 12;
            const glm::mat4 frame = keeper.GetParent().GetTransform().GetTransform() * keeper.GetTransform().GetTransform();
            const glm::vec3 here(frame[3]), starboard(frame[0]), ahead(frame[2]);
            const auto placeBoat = [&](f32 range)
            { boat.GetComponent<TransformComponent>().Translation = here + range * starboard; };
            placeBoat(200);
            scene.PropagateWorldTransforms();

            LuaScriptEngine::Init();
            LuaScriptEngine::OnRuntimeStart(&scene);
            const struct EndPlay
            {
                Scene& Playing;
                Scene* Previous = ScriptEngine::GetSceneContext();
                ~EndPlay()
                {
                    Playing.SetRunning(false);
                    LuaScriptEngine::OnRuntimeStop();
                    ScriptEngine::SetSceneContextForTesting(Previous);
                }
            } endPlay{ scene };
            ScriptEngine::SetSceneContextForTesting(&scene);
            scene.SetRunning(true);
            LuaScriptEngine::OnCreateEntity(keeper, script.string());
            const auto& anim = keeper.GetComponent<AnimationStateComponent>();
            const auto& ik = keeper.GetComponent<IKTargetComponent>();
            const auto& face = keeper.GetComponent<MorphTargetComponent>();
            const auto& hair = foundation.GetComponent<MorphTargetComponent>();
            const auto& bulb = lamp.GetComponent<PointLightComponent>();
            const auto clip = [&]() -> std::string
            { return anim.m_CurrentClip ? anim.m_CurrentClip->Name : ""; };
            const auto step = [&]()
            { scene.OnUpdateRuntime(Timestep(0.04f)); };
            const auto advance = [&](u32 frames)
            {
                for (u32 i = 0; i < frames; ++i)
                    step();
            };
            advance(15);
            EXPECT_EQ(clip(), "Idle");
            EXPECT_LT(ik.AimWeight, 0.02f);
            EXPECT_NEAR(bulb.m_Intensity, 0, 0.001f);

            placeBoat(50);
            advance(20);
            EXPECT_EQ(clip(), "Wave");
            EXPECT_GT(ik.AimWeight, 0.9f);
            const glm::vec3 gaze = glm::normalize(ik.AimTarget - (here + glm::vec3(0, 1.63f, 0)));
            EXPECT_GT(glm::dot(gaze, starboard), 0.6f);
            EXPECT_GT(glm::dot(gaze, ahead), 0.35f); // neck stops short of a right angle
            EXPECT_GT(left.GetTransform().GetRotationEuler().y, 0.1f);
            EXPECT_GT(right.GetTransform().GetRotationEuler().y, 0.1f);
            EXPECT_GT(face.GetWeight("MorphTarget_3"), 0.3f);
            EXPECT_NEAR(hair.GetWeight("MorphTarget_3"), face.GetWeight("MorphTarget_3"), 1e-5f);
            f32 peakBlink = 0;
            for (u32 i = 0; i < 100; ++i)
            {
                step();
                const f32 blink = face.GetWeight("MorphTarget_0");
                peakBlink = std::max(peakBlink, blink);
                EXPECT_NEAR(face.GetWeight("MorphTarget_1"), blink, 1e-5f);
                EXPECT_NEAR(hair.GetWeight("MorphTarget_0"), blink, 1e-5f);
                EXPECT_NEAR(hair.GetWeight("MorphTarget_1"), blink, 1e-5f);
            }
            EXPECT_GT(peakBlink, 0.75f);
            EXPECT_LT(face.GetWeight("MorphTarget_0"), 0.01f);
            EXPECT_EQ(clip(), "Idle");
            EXPECT_LT(face.GetWeight("MorphTarget_3"), 0.01f);

            // Crossing the greeting radius again while loitering cannot retrigger
            // the wave. Leaving the outer radius starts a distinct approach.
            placeBoat(70);
            advance(3);
            placeBoat(50);
            advance(15);
            EXPECT_EQ(clip(), "Idle");
            placeBoat(80);
            advance(3);
            placeBoat(50);
            advance(15);
            EXPECT_EQ(clip(), "Wave");

            for (f32 hour : { 18.6f, 2.0f })
            {
                clock.m_TimeOfDayHours = hour;
                advance(15);
                EXPECT_EQ(clip(), "LanternRaise") << hour;
                EXPECT_GT(bulb.m_Intensity, 1.0f);
                const glm::vec3 socket(scene.GetWorldTransform(static_cast<entt::entity>(wick))[3]);
                EXPECT_LT(glm::distance(lamp.GetTransform().Translation, socket), 0.1f);
            }
            placeBoat(200);
            clock.m_TimeOfDayHours = 9;
            advance(15);
            EXPECT_EQ(clip(), "Idle") << "a wave interrupted by night must not replay after the boat leaves";
            advance(85);
            EXPECT_EQ(clip(), "Idle");
            EXPECT_LT(ik.AimWeight, 0.02f);
            EXPECT_NEAR(bulb.m_Intensity, 0, 0.001f);

            // Save after the gesture finishes, with the same boat still inside
            // the greeting radius. Continue reconstructs Lua's instance table.
            placeBoat(50);
            advance(15);
            ASSERT_EQ(clip(), "Wave");
            advance(100);
            ASSERT_EQ(clip(), "Idle");
            const auto saved = SaveGameSerializer::CaptureSceneState(scene);
            ASSERT_FALSE(saved.empty());
            scene.SetRunning(false);
            LuaScriptEngine::OnRuntimeStop();
            ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(scene, saved));
            Entity restoredKeeper = scene.FindEntityByName(kKeeper);
            ASSERT_TRUE(restoredKeeper);
            const glm::vec3 restoredFoot(scene.GetWorldTransform(static_cast<entt::entity>(restoredKeeper))[3]);
            EXPECT_LT(glm::distance(restoredFoot, here), 1e-3f) << "restore must publish world space before the first script reads it";
            LuaScriptEngine::OnRuntimeStart(&scene);
            ScriptEngine::SetSceneContextForTesting(&scene);
            scene.SetRunning(true);
            LuaScriptEngine::OnCreateEntity(restoredKeeper, script.string());
            advance(20);
            const auto& resumed = restoredKeeper.GetComponent<AnimationStateComponent>();
            ASSERT_TRUE(resumed.m_CurrentClip);
            EXPECT_EQ(resumed.m_CurrentClip->Name, "Idle") << "Continue is not another approach";
            EXPECT_GT(restoredKeeper.GetComponent<IKTargetComponent>().AimWeight, 0.9f);
        }

        class DriftKeeperDeliveryTest : public RendererAttachedTest
        {
            void BuildScene() override {}
        };

        TEST_F(DriftKeeperDeliveryTest, EveryLevelAndItsProfilesSurviveAColdPackLoad)
        {
            OLO_ENSURE_GPU_OR_SKIP();
            const Json rig = Json::parse(std::ifstream(kSandbox / kModelDir / "Keeper.rig.json"));
            const fs::path source = TempDir() / "source";
            const fs::path package = TempDir() / "package";
            const fs::path pack = package / "Assets/AssetPack.olopack";
            fs::create_directories(source / kScenePath.parent_path());
            {
                const auto authored = YAML::LoadFile((kSandbox / kScenePath).string());
                ASSERT_TRUE(FindTag(authored, kKeeper) && FindTag(authored, kTower));
                std::ofstream sceneFile(source / kScenePath);
                sceneFile << KeeperSlice(authored);
                ASSERT_TRUE(sceneFile.good());
            }
            sizet copied = 0;
            std::vector<std::string> unresolved;
            std::string error;
            ASSERT_TRUE(StageSceneReferencedContent({ source / kScenePath }, kSandbox, "Assets", OLO_TEST_EDITOR_ROOT,
                                                    source, copied, unresolved, error))
                << error;
            ASSERT_TRUE(unresolved.empty()) << ::testing::PrintToString(unresolved);

            AssetRegistry shipped, selected;
            ASSERT_TRUE(shipped.Deserialize(kSandbox / "AssetRegistry.oar"));
            std::set<fs::path> profiles;
            for (const auto& profile : rig.at("profiles"))
                profiles.emplace(profile.get<std::string>());
            for (const auto& metadata : shipped.GetAllAssets())
            {
                const auto& path = metadata.FilePath;
                if (path == kScenePath)
                    selected.AddAsset(metadata);
                else if (path.parent_path() == kModelDir || path.parent_path() == "Assets/Grooms/DriftKeeper" || profiles.contains(path))
                {
                    selected.AddAsset(metadata);
                    fs::create_directories((source / path).parent_path());
                    fs::copy_file(kSandbox / path, source / path, fs::copy_options::overwrite_existing);
                }
            }
            // Handle-addressed far levels are absent from the scene path walk.
            // Their external glTF buffers accompany them into the cook.
            for (const auto& file : fs::directory_iterator(kSandbox / kModelDir))
                if (file.path().extension() == ".bin")
                    fs::copy_file(file.path(), source / kModelDir / file.path().filename(), fs::copy_options::overwrite_existing);
            ASSERT_TRUE(selected.Serialize(source / "AssetRegistry.oar"));
            const struct RestoreProject
            {
                fs::path WorkingDirectory = fs::current_path();
                Ref<Project> Previous = Project::GetActive();
                Ref<AssetManagerBase> Manager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
                ~RestoreProject()
                {
                    fs::current_path(WorkingDirectory);
                    Project::RestoreActive(Previous, Manager);
                }
            } restore;
            ProjectConfig config;
            config.Name = "DriftKeeper";
            config.AssetDirectory = "Assets";
            config.StartScene = "Scenes/Drift.olo";
            fs::current_path(source);
            ASSERT_TRUE(Project::NewInMemory(source, config));
            {
                auto editor = Ref<EditorAssetManager>::Create();
                editor->Initialize(false);
                Project::SetAssetManager(editor);
                AssetPackBuilder::BuildSettings settings;
                settings.m_OutputPath = pack;
                settings.m_IncludeScriptModule = false;
                settings.m_IncludeLocalizationFiles = false;
                std::atomic<f32> progress{ 0.0f };
                const auto result = AssetPackBuilder::BuildFromRegistry(selected, settings, progress);
                ASSERT_TRUE(result.m_Success) << result.m_ErrorMessage;
                ASSERT_EQ(result.m_FailedAssetCount, 0u);
                ASSERT_EQ(result.m_SceneCount, 1u);
            }
            ASSERT_TRUE(StageSceneReferencedContent({ source / kScenePath }, source, "Assets", OLO_TEST_EDITOR_ROOT,
                                                    package, copied, unresolved, error))
                << error;
            ASSERT_TRUE(unresolved.empty()) << ::testing::PrintToString(unresolved);
            Project::Unload();
            fs::current_path(package);
            fs::remove_all(source);
            ASSERT_TRUE(Project::NewInMemory(package, config));
            auto runtime = Ref<RuntimeAssetManager>::Create(false);
            ASSERT_TRUE(runtime->LoadAssetPack(pack));
            Project::SetAssetManager(runtime);
            auto scene = runtime->GetAsset(selected.GetHandleFromPath(kScenePath)).As<Scene>();
            ASSERT_TRUE(scene);
            Entity body = scene->FindEntityByName(kKeeper);
            ASSERT_TRUE(body);
            ASSERT_TRUE(body.HasComponent<AnimationStateComponent>());
            const auto& anim = body.GetComponent<AnimationStateComponent>();
            EXPECT_EQ(fs::weakly_canonical(anim.m_SourceFilePath), fs::weakly_canonical(package / kModelDir / "Keeper.gltf"));
            std::set<std::string> names;
            for (const auto& clip : anim.m_AvailableClips)
            {
                ASSERT_TRUE(clip);
                names.insert(clip->Name);
            }
            EXPECT_EQ(names, rig.at("clips").get<std::set<std::string>>());

            ASSERT_TRUE(body.HasComponent<LODGroupComponent>());
            const auto& group = body.GetComponent<LODGroupComponent>();
            ASSERT_TRUE(group.m_Enabled);
            ASSERT_FALSE(group.m_AutoGenerated);
            ASSERT_EQ(group.m_LODGroup.Levels.Num(), 3);
            ASSERT_EQ(rig.at("lods").size(), 3u);
            u32 previousCount = std::numeric_limits<u32>::max();
            for (i32 i = 0; i < group.m_LODGroup.Levels.Num(); ++i)
            {
                const auto& authored = rig.at("lods").at(static_cast<sizet>(i));
                const fs::path path = kModelDir / authored.at("source").get<std::string>();
                SCOPED_TRACE(path.generic_string());
                const auto& level = group.m_LODGroup.Levels[i];
                EXPECT_NEAR(level.MaxDistance, authored.at("maxDistance").get<f32>(), 1e-3f);
                const auto mesh = runtime->GetAsset(level.MeshHandle).As<Mesh>();
                ASSERT_TRUE(mesh && mesh->GetMeshSource());
                const auto& surface = mesh->GetMeshSource();
                EXPECT_EQ(surface->GetHandle(), selected.GetHandleFromPath(path));
                ASSERT_NO_FATAL_FAILURE(CheckLevel(surface, Json::parse(std::ifstream(kSandbox / path)), rig.at("morphs")));
                const u32 count = static_cast<u32>(surface->GetIndices().Num() / 3);
                EXPECT_EQ(level.TriangleCount, count);
                EXPECT_GT(count, 0u);
                EXPECT_LT(count, previousCount);
                previousCount = count;
            }

            ASSERT_TRUE(body.HasComponent<MaterialOverridesComponent>());
            ASSERT_TRUE(body.HasComponent<MeshComponent>() && body.GetComponent<MeshComponent>().m_MeshSource);
            const auto& bodyMesh = body.GetComponent<MeshComponent>().m_MeshSource;
            std::set<fs::path> used;
            for (const auto& patch : body.GetComponent<MaterialOverridesComponent>().m_Overrides)
            {
                if (patch.Kind != MaterialKind::Skin)
                    continue;
                const auto& materials = bodyMesh->GetImportedMaterials();
                EXPECT_TRUE(std::ranges::any_of(materials, [&](const auto& material)
                                                { return material && material->GetName() == patch.MaterialName; }))
                    << patch.MaterialName.ToView();
                const auto path = selected.GetMetadata(patch.SkinProfile).FilePath;
                EXPECT_EQ(path.generic_string(), rig.at("profiles").at(std::string(patch.MaterialName.ToView())).get<std::string>());
                used.insert(path);
            }
            ASSERT_TRUE(body.HasComponent<SkeletonComponent>() && body.GetComponent<SkeletonComponent>().m_Skeleton);
            const auto& skeleton = *body.GetComponent<SkeletonComponent>().m_Skeleton;
            for (const auto& socket : rig.at("eyes"))
            {
                const auto bone = socket.at("bone").get<std::string>();
                Entity eye = scene->FindEntityByName(bone == "eye_L" ? "Keeper Eye L" : "Keeper Eye R");
                ASSERT_TRUE(eye && eye.HasComponent<MaterialComponent>() && eye.HasComponent<MeshComponent>());
                const auto joint = std::ranges::find(skeleton.m_BoneNames, bone);
                ASSERT_NE(joint, skeleton.m_BoneNames.end()) << bone;
                const auto index = static_cast<sizet>(joint - skeleton.m_BoneNames.begin());
                ASSERT_LT(index, skeleton.m_InverseBindPoses.size());
                const auto centre = glm::inverse(skeleton.m_InverseBindPoses[index]) *
                                    glm::vec4(eye.GetComponent<TransformComponent>().Translation, 1.0f);
                for (int axis = 0; axis < 3; ++axis)
                    EXPECT_NEAR(centre[axis], socket.at("centre").at(axis).get<f32>(), 1e-4f);
                const auto& globe = eye.GetComponent<MeshComponent>().m_MeshSource;
                ASSERT_TRUE(globe);
                EXPECT_EQ(globe->GetHandle(), selected.GetHandleFromPath(kModelDir / "KeeperEyeball.gltf"));
                const auto profileHandle = eye.GetComponent<MaterialComponent>().m_Material.GetSkinProfileHandle();
                const auto asset = runtime->GetAsset(profileHandle);
                ASSERT_TRUE(asset && asset->GetAssetType() == AssetType::SkinProfile);
                EXPECT_GT(asset.As<SkinProfile>()->GetParameters().Ocular.OcularStrength, 0.0f);
                used.insert(selected.GetMetadata(profileHandle).FilePath);
            }
            EXPECT_EQ(used, profiles);
            EXPECT_GE(used.size(), 6u);
            for (const auto& path : used)
            {
                const auto asset = runtime->GetAsset(selected.GetHandleFromPath(path));
                ASSERT_TRUE(asset && asset->GetAssetType() == AssetType::SkinProfile) << path.string();
                EXPECT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(asset)) << path.string();
            }
            const Entity groom = scene->FindEntityByName("Keeper Groom");
            ASSERT_TRUE(groom && groom.HasComponent<GroomComponent>() && groom.HasComponent<GroomBindingComponent>());
            const auto hair = runtime->GetAsset(groom.GetComponent<GroomComponent>().m_Groom);
            ASSERT_TRUE(hair && hair->GetAssetType() == AssetType::Groom);
            EXPECT_GT(hair.As<GroomAsset>()->GetCurveCount(), 0u);
            const auto binding = runtime->GetAsset(groom.GetComponent<GroomBindingComponent>().m_Binding);
            ASSERT_TRUE(binding && binding->GetAssetType() == AssetType::GroomBinding);
            EXPECT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(binding));
            EXPECT_EQ(binding.As<GroomBindingAsset>()->GetRootCount(), hair.As<GroomAsset>()->GetCurveCount());
            ASSERT_NO_FATAL_FAILURE(CheckGreetingInPlay(*scene, body));
        }
    } // namespace
} // namespace OloEngine::Tests
