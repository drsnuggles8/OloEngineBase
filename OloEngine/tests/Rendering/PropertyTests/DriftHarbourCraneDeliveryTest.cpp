// OLO_TEST_LAYER: L8
// The two objects a sailor sees must still have geometry and all their material
// maps when the shipping runtime has only the pack. The glTF accessors and
// material bindings are the independent reference for both import paths.
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;

        const fs::path kModelDir = "Assets/Models/DriftHarbourCrane";
        const fs::path kScenePath = "Assets/Scenes/Drift.olo";

        struct CargoAsset
        {
            const char* Tag;
            const char* Filename;
        };
        constexpr CargoAsset kAssets[] = {
            { "Ridgeback - hand crane", "HarbourCrane.gltf" },
            { "Ridgeback - timber landing", "HarbourFooting.gltf" },
        };

        void CheckTexture(const Json& gltf, const Json& binding, const Ref<Texture2D>& texture, const AssetRegistry& registry, bool packed)
        {
            const auto image = gltf.at("textures").at(binding.at("index").get<sizet>()).at("source").get<sizet>();
            const auto file = kModelDir / gltf.at("images").at(image).at("uri").get<std::string>();
            SCOPED_TRACE(file.generic_string());
            const auto handle = registry.GetHandleFromPath(file);
            ASSERT_NE(static_cast<u64>(handle), 0u);
            ASSERT_TRUE(texture && texture->IsLoaded());
            EXPECT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(texture));
            // On a loose import the texture may be opened directly by path. A
            // packed texture can instead have just its original registry handle.
            if (packed)
                EXPECT_EQ(texture->GetHandle(), handle);
            else
                EXPECT_TRUE(texture->GetHandle() == handle || fs::path(texture->GetPath()).filename() == file.filename());
            EXPECT_GT(texture->GetWidth(), 1u);
            EXPECT_GT(texture->GetHeight(), 1u);
        }

        void CheckMesh(const Ref<MeshSource>& mesh, const Json& gltf, const AssetRegistry& registry, bool packed)
        {
            ASSERT_TRUE(mesh);
            ASSERT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(mesh));
            const auto& primitives = gltf.at("meshes").at(0).at("primitives");
            const auto& submeshes = mesh->GetSubmeshes();
            ASSERT_EQ(static_cast<sizet>(submeshes.Num()), primitives.size());
            glm::vec3 low(std::numeric_limits<f32>::max()), high(std::numeric_limits<f32>::lowest());
            sizet checkedMaps = 0;
            for (const auto& primitive : primitives)
            {
                const auto& material = gltf.at("materials").at(primitive.at("material").get<sizet>());
                const auto name = material.at("name").get<std::string>();
                SCOPED_TRACE(name);
                auto index = 0;
                for (; index < submeshes.Num(); ++index)
                {
                    const auto imported = mesh->GetImportedMaterialForSubmesh(index);
                    if (imported && imported->GetName().ToView() == name)
                        break;
                }
                ASSERT_LT(index, submeshes.Num());
                auto imported = mesh->GetImportedMaterialForSubmesh(index);
                const auto& positions = gltf.at("accessors").at(primitive.at("attributes").at("POSITION").get<sizet>());
                for (int axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], positions.at("min").at(axis).get<f32>());
                    high[axis] = std::max(high[axis], positions.at("max").at(axis).get<f32>());
                }
                EXPECT_EQ(submeshes[index].m_IndexCount, gltf.at("accessors").at(primitive.at("indices").get<sizet>()).at("count").get<u32>());
                const auto& pbr = material.at("pbrMetallicRoughness");
                auto check = [&](const Json& properties, const char* slot, const Ref<Texture2D>& texture)
                {
                    if (auto it = properties.find(slot); it != properties.end())
                    {
                        ++checkedMaps;
                        CheckTexture(gltf, *it, texture, registry, packed);
                    }
                };
                check(pbr, "baseColorTexture", imported->GetAlbedoMap());
                check(pbr, "metallicRoughnessTexture", imported->GetMetallicRoughnessMap());
                check(material, "normalTexture", imported->GetNormalMap());
                check(material, "occlusionTexture", imported->GetAOMap());
                check(material, "emissiveTexture", imported->GetEmissiveMap());
            }
            EXPECT_GT(checkedMaps, 0u);
            for (int axis = 0; axis < 3; ++axis)
            {
                EXPECT_NEAR(mesh->GetBoundingBox().Min[axis], low[axis], 0.001f);
                EXPECT_NEAR(mesh->GetBoundingBox().Max[axis], high[axis], 0.001f);
            }
        }

        class DriftHarbourCraneDelivery : public RendererAttachedTest
        {
            void BuildScene() override {}
        };

        TEST_F(DriftHarbourCraneDelivery, ThePlacedCraneAndLandingKeepTheirGeometryAndFinishesInAPack)
        {
            OLO_ENSURE_GPU_OR_SKIP();
            const auto sandbox = fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject";
            const auto root = TempDir() / "source";
            const auto package = TempDir() / "shipping";
            const auto pack = package / "Assets/AssetPack.olopack";
            fs::create_directories(root / kScenePath.parent_path());
            fs::create_directories(root / kModelDir);

            AssetRegistry authored, selected;
            ASSERT_TRUE(authored.Deserialize(sandbox / "AssetRegistry.oar"));
            for (const auto& metadata : authored.GetAllAssets())
            {
                if (metadata.FilePath == kScenePath || metadata.FilePath.parent_path() == kModelDir)
                    selected.AddAsset(metadata);
            }
            for (const auto& file : fs::directory_iterator(sandbox / kModelDir))
            {
                const auto ext = file.path().extension();
                if (ext == ".gltf" || ext == ".bin" || ext == ".png")
                    fs::copy_file(file.path(), root / kModelDir / file.path().filename());
            }
            ASSERT_TRUE(selected.Serialize(root / "AssetRegistry.oar"));
            const auto authoredScene = YAML::LoadFile((sandbox / kScenePath).string());
            YAML::Node slice;
            slice["Scene"] = authoredScene["Scene"];
            slice["Version"] = authoredScene["Version"];
            for (const auto& entity : authoredScene["Entities"])
            {
                for (const auto& asset : kAssets)
                    if (entity["TagComponent"] && entity["TagComponent"]["Tag"].as<std::string>() == asset.Tag)
                        slice["Entities"].push_back(entity);
            }
            ASSERT_EQ(slice["Entities"].size(), std::size(kAssets));
            {
                std::ofstream yaml(root / kScenePath);
                yaml << slice;
                ASSERT_TRUE(yaml.good());
            }
            // Drop loaded GPU assets before the renderer fixture tears down.
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
            config.AssetDirectory = "Assets";
            config.StartScene = "Scenes/Drift.olo";
            fs::current_path(root);
            ASSERT_TRUE(Project::NewInMemory(root, config));
            {
                auto editor = Ref<EditorAssetManager>::Create();
                editor->Initialize(false);
                Project::SetAssetManager(editor);
                for (const auto& asset : kAssets)
                {
                    SCOPED_TRACE(asset.Filename);
                    const auto path = kModelDir / asset.Filename;
                    const auto mesh = editor->GetAsset(selected.GetHandleFromPath(path)).As<MeshSource>();
                    ASSERT_NO_FATAL_FAILURE(CheckMesh(mesh, Json::parse(std::ifstream(sandbox / path)), selected, false));
                }
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
            Project::Unload();
            fs::current_path(package);
            fs::remove_all(root);
            ASSERT_TRUE(Project::NewInMemory(package, config));
            auto runtime = Ref<RuntimeAssetManager>::Create(false);
            ASSERT_TRUE(runtime->LoadAssetPack(pack));
            Project::SetAssetManager(runtime);
            const auto scene = runtime->GetAsset(selected.GetHandleFromPath(kScenePath)).As<Scene>();
            ASSERT_TRUE(scene);
            for (const auto& asset : kAssets)
            {
                SCOPED_TRACE(asset.Tag);
                Entity entity = scene->FindEntityByName(asset.Tag);
                ASSERT_TRUE(entity);
                ASSERT_TRUE(entity.HasComponent<MeshComponent>());
                const auto& mesh = entity.GetComponent<MeshComponent>().m_MeshSource;
                const auto model = kModelDir / asset.Filename;
                ASSERT_TRUE(mesh);
                EXPECT_EQ(mesh->GetHandle(), selected.GetHandleFromPath(model));
                ASSERT_NO_FATAL_FAILURE(CheckMesh(mesh, Json::parse(std::ifstream(sandbox / model)), selected, true));
                for (const auto& authoredEntity : slice["Entities"])
                {
                    if (authoredEntity["TagComponent"]["Tag"].as<std::string>() != asset.Tag)
                        continue;
                    const auto placement = authoredEntity["TransformComponent"];
                    const auto& tc = entity.GetComponent<TransformComponent>();
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        EXPECT_NEAR(tc.Translation[axis], placement["Translation"][axis].as<f32>(), 0.001f);
                        EXPECT_NEAR(tc.GetRotationEuler()[axis], placement["Rotation"][axis].as<f32>(), 0.001f);
                        EXPECT_NEAR(tc.Scale[axis], placement["Scale"][axis].as<f32>(), 0.001f);
                    }
                }
            }
        }
    } // namespace
} // namespace OloEngine::Tests
