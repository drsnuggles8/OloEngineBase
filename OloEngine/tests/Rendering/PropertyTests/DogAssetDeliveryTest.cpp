// OLO_TEST_LAYER: L8
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cstdlib>
#include <filesystem>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    class DogAssetDelivery : public RendererAttachedTest, public ::testing::WithParamInterface<std::string>
    {
        void BuildScene() override {}
    };

    INSTANTIATE_TEST_SUITE_P(Breeds, DogAssetDelivery, ::testing::Values("Samoyed", "Bernese"),
                             [](const ::testing::TestParamInfo<std::string>& info)
                             { return info.param; });

    TEST_P(DogAssetDelivery, TheAuthoredSceneAndAllItsAssetsSurvivePackaging)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto editorRoot = fs::path(OLO_TEST_EDITOR_ROOT);
        const auto sandbox = editorRoot / "SandboxProject";
        const std::string breed = GetParam();
        const auto projectRoot = TempDir("dog-delivery-" + breed);
        const auto scenePath = fs::path("Assets/Scenes") / (breed + ".olo");
        const auto modelPath = fs::path("Assets/Models") / breed;
        const auto groomPath = fs::path("Assets/Grooms") / breed;

        struct RestoreProject
        {
            Ref<Project> Previous = Project::GetActive();
            Ref<AssetManagerBase> Assets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            fs::path WorkingDirectory = fs::current_path();
            ~RestoreProject()
            {
                fs::current_path(WorkingDirectory);
                Project::Unload();
                if (Previous)
                    Project::NewInMemory(Previous->GetDirectory(), Previous->GetConfig());
                if (Assets)
                    Project::SetAssetManager(Assets);
            }
        } restore;

        // Keep the authored scene and stable asset handles. Stage all path-based
        // references with the same dependency walker Build Game uses, then copy
        // the registered assets (including eye meshes referenced only by handle).
        fs::create_directories(projectRoot / scenePath.parent_path());
        fs::copy_file(sandbox / scenePath, projectRoot / scenePath);
        sizet copied = 0;
        std::vector<std::string> unresolved; // Production API boundary.
        std::string error;
        ASSERT_TRUE(StageSceneReferencedContent({ sandbox / scenePath }, sandbox, "Assets", editorRoot,
                                                projectRoot, copied, unresolved, error))
            << error;
        ASSERT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
        for (const auto& file : fs::directory_iterator(sandbox / modelPath))
        {
            const auto extension = file.path().extension();
            if (extension == ".gltf" || extension == ".bin" || extension == ".png" || extension == ".oloskin")
            {
                fs::create_directories(projectRoot / modelPath);
                fs::copy_file(file.path(), projectRoot / modelPath / file.path().filename(), fs::copy_options::overwrite_existing);
            }
        }
        AssetRegistry shipped;
        ASSERT_TRUE(shipped.Deserialize(sandbox / "AssetRegistry.oar"));
        AssetRegistry selected;
        for (const auto& metadata : shipped.GetAllAssets())
        {
            const auto path = metadata.FilePath.generic_string();
            if (metadata.FilePath != scenePath && !path.starts_with(modelPath.generic_string() + "/") &&
                !path.starts_with(groomPath.generic_string() + "/") && !path.starts_with("Assets/Materials/Dog"))
                continue;
            selected.AddAsset(metadata);
            fs::create_directories((projectRoot / metadata.FilePath).parent_path());
            fs::copy_file(sandbox / metadata.FilePath, projectRoot / metadata.FilePath, fs::copy_options::overwrite_existing);
        }
        const AssetHandle sceneHandle = selected.GetHandleFromPath(scenePath);
        ASSERT_NE(static_cast<u64>(sceneHandle), 0u);
        ASSERT_TRUE(selected.Serialize(projectRoot / "AssetRegistry.oar"));
        ProjectConfig config;
        config.Name = breed;
        config.AssetDirectory = "Assets";
        config.StartScene = "Scenes/" + breed + ".olo";
        ASSERT_TRUE(Project::NewInMemory(projectRoot, config));
        auto editorAssets = Ref<EditorAssetManager>::Create();
        editorAssets->Initialize(false);
        Project::SetAssetManager(editorAssets);

        fs::path package = projectRoot / "Package";
        const char* retain = std::getenv("OLO_DOG_PACKAGE_DIR");
        std::atomic<f32> progress{ 0.0f };
        if (retain != nullptr && retain[0] != '\0')
        {
            GameBuildSettings settings;
            settings.GameName = breed;
            settings.OutputDirectory = fs::absolute(retain);
            settings.IncludeScriptModule = false;
            settings.StartScene = config.StartScene;
            package = settings.OutputDirectory / breed;
            ASSERT_FALSE(fs::exists(package)) << "choose a new package destination";
            fs::current_path(editorRoot); // Build Game resolves engine resources here.
            const auto built = GameBuildPipeline::Build(settings, progress);
            ASSERT_TRUE(built.Success) << built.ErrorMessage;
            EXPECT_EQ(built.SceneCount, 1u);
        }
        else
        {
            AssetPackBuilder::BuildSettings settings;
            settings.m_OutputPath = package / "Assets/AssetPack.olopack";
            settings.m_IncludeScriptModule = false;
            settings.m_IncludeLocalizationFiles = false;
            const auto packed = AssetPackBuilder::BuildFromRegistry(selected, settings, progress);
            ASSERT_TRUE(packed.m_Success) << packed.m_ErrorMessage;
            EXPECT_EQ(packed.m_FailedAssetCount, 0u);
            EXPECT_EQ(packed.m_SceneCount, 1u);
            ASSERT_TRUE(StageSceneReferencedContent({ projectRoot / scenePath }, projectRoot, "Assets", editorRoot,
                                                    package, copied, unresolved, error))
                << error;
            EXPECT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
        }

        // Move resolution away from the source project before loading the pack.
        // A path that works only because the editor has its source tree must fail.
        ASSERT_TRUE(Project::NewInMemory(package, config));
        fs::current_path(package);
        auto runtime = Ref<RuntimeAssetManager>::Create(false);
        ASSERT_TRUE(runtime->LoadAssetPack(package / "Assets/AssetPack.olopack"));
        Project::SetAssetManager(runtime);
        for (const auto& metadata : selected.GetAllAssets())
        {
            const auto asset = runtime->GetAsset(metadata.Handle);
            ASSERT_TRUE(asset) << metadata.FilePath.string();
            EXPECT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(asset)) << metadata.FilePath.string();
        }
        auto scene = runtime->GetAsset(sceneHandle).As<Scene>();
        ASSERT_TRUE(scene);
        scene->SetRenderingEnabled(false);
        scene->OnUpdateRuntime(Timestep(1.0f / 60.0f));
        const auto bodies = scene->GetAllEntitiesWith<AnimationStateComponent, MorphTargetComponent>();
        ASSERT_EQ(bodies.size_hint(), 1u);
        for (const auto id : bodies)
        {
            Entity body{ id, scene.Raw() };
            const auto& morph = body.GetComponent<MorphTargetComponent>();
            ASSERT_TRUE(morph.MorphTargets);
            EXPECT_EQ(morph.MorphTargets->GetTargetCount(), breed == "Samoyed" ? 19u : 8u);
            const auto& animation = body.GetComponent<AnimationStateComponent>();
            EXPECT_EQ(animation.m_AvailableClips.size(), 6u);
            EXPECT_TRUE(fs::exists(animation.m_SourceFilePath));
            EXPECT_EQ(fs::weakly_canonical(animation.m_SourceFilePath),
                      fs::weakly_canonical(package / modelPath / (breed + ".gltf")));
        }
        const auto coats = scene->GetAllEntitiesWith<GroomComponent, GroomBindingComponent>();
        ASSERT_EQ(coats.size_hint(), 1u);
        for (const auto id : coats)
        {
            Entity coat{ id, scene.Raw() };
            const auto groom = runtime->GetAsset(coat.GetComponent<GroomComponent>().m_Groom).As<GroomAsset>();
            ASSERT_TRUE(groom);
            EXPECT_EQ(groom->GetCurveCount(), breed == "Samoyed" ? 1'999'138u : 1'615'603u);
            EXPECT_TRUE(runtime->GetAsset(coat.GetComponent<GroomBindingComponent>().m_Binding));
        }
    }
} // namespace OloEngine::Tests
