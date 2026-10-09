// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomStreaming.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/Foliage/FoliageStreamingPayload.h"
#include "Groom/GroomLodFixture.h"
#include "Groom/GroomStrandFixture.h"
#include "TestAsyncLoadHooks.h"
#include "TestOptions.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;
    constexpr u64 kCoatEntity = 1257001;

    struct ContextGuard
    {
        Ref<Project> PreviousProject = Project::GetActive();
        Ref<AssetManagerBase> PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
        fs::path PreviousDirectory = fs::current_path();
        u64 PreviousStagingBudget = RepresentationStreaming::Get().GetStats().MaxStagingCpuBytes;
        ~ContextGuard()
        {
            fs::current_path(PreviousDirectory);
            Project::Unload();
            if (PreviousProject)
                Project::NewInMemory(PreviousProject->GetDirectory(), PreviousProject->GetConfig());
            if (PreviousAssets)
                Project::SetAssetManager(PreviousAssets);
            RepresentationStreaming::Get().SetStagingBudget(PreviousStagingBudget);
        }
    };

    struct GateGuard
    {
        Tasks::FTaskEvent Gate;
        ~GateGuard()
        {
            Gate.Trigger();
        }
    };

    template<typename Predicate>
    bool Until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::yield();
        }
        return predicate();
    }

    Ref<Scene> MakeRuntimeScene(AssetHandle groom)
    {
        auto scene = Ref<Scene>::Create();
        scene->SetName("ResidencyStreaming");
        auto& settings = scene->GetStreamingSettings();
        settings.RepresentationResidentMegabytes = 16.0f;
        settings.RepresentationUploadMegabytesPerFrame = 2.0f;
        settings.RepresentationStagingMegabytes = 32.0f;
        Entity coat = scene->CreateEntityWithUUID(OloEngine::UUID(kCoatEntity), "PackedCoat");
        coat.GetComponent<TransformComponent>().Translation = { -1.0f, 1.0f, 0.0f };
        coat.GetComponent<TransformComponent>().Scale = glm::vec3(5.0f);
        auto& render = coat.AddComponent<GroomComponent>();
        render.m_Groom = groom;
        render.m_RenderStrands = true;
        render.m_ShowPreview = false;
        render.m_MaxRenderStrands = 2000;
        coat.AddComponent<GroomLodComponent>().m_CardPixelSize = 64.0f;

        Entity ground = scene->CreateEntityWithUUID(OloEngine::UUID(1257002), "StagedPlants");
        auto& terrain = ground.AddComponent<TerrainComponent>();
        terrain.m_CollisionEnabled = false;
        terrain.m_ProceduralEnabled = true;
        terrain.m_ProceduralResolution = 32;
        terrain.m_ProceduralOctaves = 1;
        terrain.m_HeightScale = 0.001f;
        terrain.m_WorldSizeX = 12.0f;
        terrain.m_WorldSizeZ = 12.0f;
        terrain.m_TessellationEnabled = false;
        FoliageLayer plants;
        plants.Name = "StagedLeafFixture";
        plants.MeshPath = "Assets/Plants/leaf.obj";
        plants.AlbedoPath = "Assets/Plants/leaf.png";
        plants.SplatmapChannel = -1;
        plants.Density = 0.4f;
        plants.MinHeight = 0.8f;
        plants.MaxHeight = 1.2f;
        plants.MeshViewDistance = 20.0f;
        plants.MeshFadeStartDistance = 15.0f;
        ground.AddComponent<FoliageComponent>().m_Layers.Add(std::move(plants));

        Entity camera = scene->CreateEntityWithUUID(OloEngine::UUID(1257003), "RuntimeCamera");
        camera.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 6.0f };
        camera.GetComponent<TransformComponent>().SetRotationEuler({ -0.20f, 0.0f, 0.0f });
        camera.AddComponent<CameraComponent>().Camera.SetPerspective(glm::radians(55.0f), 0.05f, 100.0f);
        scene->CreateEntity("Sun").AddComponent<DirectionalLightComponent>().m_Intensity = 3.0f;
        return scene;
    }
} // namespace

// This verifies the real builder/manager/staging seams on CPU. The exported
// fixture is for a separate OloRuntime launch; this test never claims GPU proof.
TEST(RuntimeRepresentationStreamingPackTest, PackedGroomKeepsCardsWhileFinePreparationWaitsAndPlantsResolveFromTheGame)
{
#if !OLO_ASYNC_ASSETS
    GTEST_SKIP() << "The runtime pack async seam requires OLO_ASYNC_ASSETS";
#endif
    Tests::EnsureTaskSchedulerStarted();
    ContextGuard restore;
    const fs::path exportRoot = Tests::Options().CaptureOutDir.empty() ? fs::path{} : fs::absolute(Tests::Options().CaptureOutDir);
    const fs::path root = Tests::TempDir("runtime-representations");
    const fs::path source = root / "Source";
    const fs::path game = root / "Game";
    fs::create_directories(source / "Assets/Grooms");
    fs::create_directories(source / "Assets/Scenes");
    fs::create_directories(source / "Assets/Plants");
    fs::create_directories(game / "Assets");
    std::ofstream(source / "Residency.oloproj") << "Project:\n  Name: ResidencyStreaming\n"
                                                   "  StartScene: Scenes/Residency.olo\n  AssetDirectory: Assets\n  ScriptModulePath: \"\"\n";

    auto fixture = Tests::GroomStrandFixture::MakePelt(2000, 4);
    ASSERT_TRUE(fixture.Groom) << fixture.FailureReason;
    std::string reason;
    auto groom = Tests::GroomLodFixture::RebuildWithWrappedRootUVs(*fixture.Groom, reason);
    ASSERT_TRUE(groom) << reason;
    GroomLodLevel cards;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, GroomCardSettings{}, cards, reason, nullptr)) << reason;
    ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*groom, { cards }, reason)) << reason;
    std::vector<u8> cooked;
    ASSERT_TRUE(GroomSerializer::EncodeToBytes(*groom, cooked, reason)) << reason;
    {
        std::ofstream file(source / "Assets/Grooms/Coat.ologroom", std::ios::binary);
        file.write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
        ASSERT_TRUE(file);
    }
    ASSERT_TRUE(Project::Load(source / "Residency.oloproj"));
    auto editor = Ref<EditorAssetManager>::Create();
    editor->Initialize(false);
    Project::SetAssetManager(editor);
    const AssetHandle groomHandle = editor->ImportAsset(source / "Assets/Grooms/Coat.ologroom");
    ASSERT_NE(static_cast<u64>(groomHandle), 0u);
    auto scene = MakeRuntimeScene(groomHandle);
    const fs::path scenePath = source / "Assets/Scenes/Residency.olo";
    SceneSerializer(scene).Serialize(scenePath);
    const AssetHandle sceneHandle = editor->ImportAsset(scenePath);
    ASSERT_NE(static_cast<u64>(sceneHandle), 0u);
    // Persist before the builder creates its own manager: scene handles must
    // retain their registry identities, not be replaced by a later rescan.
    ASSERT_TRUE(editor->SerializeAssetRegistry());
    AssetRegistry registry;
    registry.AddAsset(editor->GetMetadata(groomHandle));
    registry.AddAsset(editor->GetMetadata(sceneHandle));
    AssetPackBuilder::BuildSettings build;
    build.m_OutputPath = game / "Assets/AssetPack.olopack";
    build.m_IncludeScriptModule = false;
    std::atomic<f32> progress{ 0.0f };
    const auto pack = AssetPackBuilder::BuildFromRegistry(registry, build, progress);
    ASSERT_TRUE(pack.m_Success) << pack.m_ErrorMessage;
    EXPECT_EQ(pack.m_FailedAssetCount, 0u);
    EXPECT_EQ(pack.m_SceneCount, 1u);

    // These are path-backed runtime dependencies, deliberately outside the
    // selected pack registry. Stage through the production dependency walker.
    std::ofstream(source / "Assets/Plants/leaf.obj") << "mtllib leaf.mtl\nusemtl leaf\n"
                                                        "v -0.4 0 0\nv 0.4 0 0\nv 0.15 1 0\nv -0.15 1 0\n"
                                                        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                                        "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    std::ofstream(source / "Assets/Plants/leaf.mtl") << "newmtl leaf\nKd 1 1 1\nmap_Kd leaf.png\n";
    constexpr std::array<u8, 16> pixels{ 40, 180, 50, 255, 50, 190, 60, 255,
                                         40, 180, 50, 255, 50, 190, 60, 255 };
    ASSERT_NE(stbi_write_png((source / "Assets/Plants/leaf.png").string().c_str(), 2, 2, 4, pixels.data(), 8), 0);
    sizet stagedCount = 0;
    std::vector<std::string> unresolved;
    ASSERT_TRUE(StageSceneReferencedContent({ scenePath }, source, "Assets", OLO_TEST_EDITOR_ROOT,
                                            game, stagedCount, unresolved, reason))
        << reason;
    EXPECT_TRUE(unresolved.empty());
    EXPECT_GE(stagedCount, 3u);

    // Make original plants unavailable while mounted as a shipped game. A
    // fallback into the authoring project can no longer make this probe pass.
    fs::rename(source / "Assets/Plants", source / "UnavailablePlants");
    auto runtime = Ref<RuntimeAssetManager>::Create(false);
    ASSERT_TRUE(runtime->LoadAssetPack(build.m_OutputPath));
    ProjectConfig runtimeConfig;
    runtimeConfig.AssetDirectory = "Assets";
    Project::NewInMemory(game, runtimeConfig);
    Project::SetAssetManager(runtime);
    fs::current_path(game);
    GateGuard packGate{ Tasks::FTaskEvent("RealGroomPackRead") };
    FRuntimeAssetLoadTestHooks hooks;
    hooks.StartGate = packGate.Gate;
    runtime->SetAsyncLoadTestHooks(std::move(hooks));
    ASSERT_FALSE(runtime->GetAssetAsync(groomHandle).IsReady);
    EXPECT_EQ(runtime->GetStreamingReport().Loads.PendingCount, 1u);
    packGate.Gate.Trigger();
    ASSERT_TRUE(Until([&]
                      {
                          runtime->SyncWithAssetThread();
                          return runtime->IsAssetLoaded(groomHandle); }));
    auto loaded = runtime->GetAsset(groomHandle).As<GroomAsset>();
    ASSERT_TRUE(loaded);
    ASSERT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(loaded));
    EXPECT_EQ(loaded->GetHandle(), groomHandle);
    EXPECT_EQ(loaded->GetCurveCount(), groom->GetCurveCount());
    ASSERT_TRUE(loaded->FindLodLevel(GroomRepresentation::Card));
    EXPECT_EQ(*loaded->FindLodLevel(GroomRepresentation::Card), cards);
    auto packedScene = runtime->GetAsset(sceneHandle).As<Scene>();
    ASSERT_TRUE(packedScene);
    EXPECT_FLOAT_EQ(packedScene->GetStreamingSettings().RepresentationResidentMegabytes, 16.0f);
    EXPECT_FLOAT_EQ(packedScene->GetStreamingSettings().RepresentationUploadMegabytesPerFrame, 2.0f);
    EXPECT_FLOAT_EQ(packedScene->GetStreamingSettings().RepresentationStagingMegabytes, 32.0f);
    const Entity packedCoat = packedScene->GetEntityByUUID(OloEngine::UUID(kCoatEntity));
    ASSERT_TRUE(packedCoat);
    EXPECT_EQ(packedCoat.GetComponent<GroomComponent>().m_Groom, groomHandle);

    RepresentationStreaming::Get().SetStagingBudget(32u * 1024u * 1024u);
    FRepresentationLoadQueue fine;
    GateGuard fineGate{ Tasks::FTaskEvent("PackedFineGeometry") };
    fine.SetStartGate(fineGate.Gate);
    GroomStrandRequest request;
    request.Groom = loaded;
    request.Handle = groomHandle;
    request.Build.MaxStrands = 2000;
    const auto descriptor = DescribeGroomGeometry(request);
    const auto prepare = [loaded, settings = request.Build]() -> Ref<FRepresentationPayload>
    { return PrepareGroomGeometry(loaded, {}, nullptr, settings, {}); };
    const u64 identity = static_cast<u64>(groomHandle);
    ASSERT_EQ(fine.Request(identity, descriptor, prepare), ERepresentationRequestResult::Queued);
    auto fallback = request;
    ApplyGroomStreamingFloor(fallback, 64);
    EXPECT_EQ(fallback.Lod.Representation, GroomRepresentation::Card);
    auto drawable = PrepareGroomGeometry(loaded, {}, fallback.LodLevel, fallback.Build, {});
    ASSERT_TRUE(drawable);
    EXPECT_GT(drawable->Vertices.Num(), 0);
    EXPECT_TRUE(fine.IsPending(identity));
    EXPECT_EQ(fine.Cancel(identity), EAssetLoadCancelResult::CancelledBeforeStart);
    fineGate.Gate.Trigger();
    fine.SetStartGate(std::nullopt);
    ASSERT_EQ(fine.Request(identity, descriptor, prepare), ERepresentationRequestResult::Queued);
    TArray<FCompletedRepresentationLoad> completed;
    ASSERT_TRUE(Until([&]
                      {
                          fine.RetrieveCompleted(completed);
                          return completed.Num() == 1; }));
    ASSERT_TRUE(completed[0].Payload);
    EXPECT_EQ(completed[0].Key, identity);
    auto prepared = completed[0].Payload.As<FPreparedGroomGeometry>();
    EXPECT_EQ(prepared->Groom->GetHandle(), groomHandle);
    EXPECT_GT(prepared->Vertices.Num(), drawable->Vertices.Num());
    prepared.Reset();
    const u64 ticket = completed[0].StagingTicket;
    completed.Reset();
    fine.ReleaseStaging(ticket);

    const fs::path plantPath = ResolveContentPath("Assets/Plants/leaf.obj");
    ASSERT_TRUE(fs::equivalent(plantPath, game / "Assets/Plants/leaf.obj"));
    auto plant = LoadFoliageStreamingPayload(plantPath);
    ASSERT_TRUE(plant);
    EXPECT_EQ(plant->Indices.Num(), 6);
    ASSERT_EQ(plant->Parts.Num(), 1);
    EXPECT_TRUE(fs::equivalent(fs::path(plant->Parts[0].AlbedoPath.ToStdString()), game / "Assets/Plants/leaf.png"));
    auto albedo = DecodeFoliageStreamingAlbedo(plant->Parts[0]);
    ASSERT_TRUE(albedo);
    EXPECT_EQ(albedo->Width, 2u);
    EXPECT_EQ(albedo->Height, 2u);
    EXPECT_EQ(albedo->Pixels.Num(), 16);
    EXPECT_GT(albedo->Pixels[1], albedo->Pixels[0]);
    albedo.Reset();
    fine.Shutdown();
    runtime->Shutdown();
    Project::SetAssetManager(editor);
    ASSERT_TRUE(Project::Load(source / "Residency.oloproj"));
    fs::rename(source / "UnavailablePlants", source / "Assets/Plants");

    if (!exportRoot.empty())
    {
        // Explicit export only: ordinary CI tests retain isolated scratch paths.
        const fs::path output = exportRoot / "cooked-fixture";
        const fs::path relativeOutput = output.lexically_relative(root);
        ASSERT_TRUE(relativeOutput.empty() || *relativeOutput.begin() == "..")
            << "Fixture export must be outside the isolated source/game tree";
        fs::current_path(OLO_TEST_EDITOR_ROOT);
        ASSERT_TRUE(GameBuildPipeline::StageRuntimeContent(game, reason)) << reason;
        std::ofstream(game / "game.manifest") << "Game:\n  Name: ResidencyStreaming\n"
                                                 "StartScene: Scenes/Scenes/Residency.olo\nRendering:\n  Is3DMode: true\n";
        fs::create_directories(output);
        fs::copy(game, output, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        fs::create_directories(output / "Source");
        fs::copy(source, output / "Source", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        std::ofstream(output / "fixture.txt") << "Generated by RuntimeRepresentationStreamingPackTest.\n"
                                                 "CPU pack/staging and pending Card geometry verified; launch OloRuntime separately for GPU evidence.\n"
                                                 "Runtime cwd: this folder. Scene: Scenes/Scenes/Residency.olo.\n";
    }
}

// Explicit evidence export only. The authored scene hydrates its real skeletal
// body on Runtime's render thread; this CPU exporter packs the immutable groom
// and binding without constructing a GPU model or modifying authoring content.
TEST(RuntimeRepresentationStreamingPackTest, ExportAuthoredBoundDogAndFernForRuntimeEvidence)
{
    if (Tests::Options().CaptureOutDir.empty())
        GTEST_SKIP() << "Use --olo-capture-out to export the authored Runtime evidence fixture";
    ContextGuard restore;
    RepresentationStreaming::Get().SetStagingBudget(4096ull * 1024u * 1024u);
    const fs::path output = fs::absolute(Tests::Options().CaptureOutDir) / "runtime-credible";
    const fs::path source = output / "Source";
    const fs::path game = output / "Game";
    const fs::path scenePath = source / "Assets/Scenes/RuntimeDogAndFern.olo";
    const fs::path editorRoot = OLO_TEST_EDITOR_ROOT;
    const fs::path fixtures = editorRoot.parent_path() / "scripts/perf/fixtures";
    fs::create_directories(scenePath.parent_path());
    fs::copy_file(fixtures / "RuntimeDogAndFern.olo", scenePath, fs::copy_options::overwrite_existing);
    fs::create_directories(source / "Assets/Scripts");
    fs::copy_file(fixtures / "ResidencyObserver.lua", source / "Assets/Scripts/ResidencyObserver.lua",
                  fs::copy_options::overwrite_existing);
    for (const fs::path& relative : { fs::path("Assets/Grooms/Dog"), fs::path("Assets/Models/Dog"),
                                      fs::path("Assets/Models/Vegetation/fern") })
    {
        fs::create_directories((source / relative).parent_path());
        fs::copy(editorRoot / "SandboxProject" / relative, source / relative,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
    std::ofstream(source / "Residency.oloproj") << "Project:\n  Name: RuntimeDogAndFern\n"
                                                   "  StartScene: Scenes/RuntimeDogAndFern.olo\n"
                                                   "  AssetDirectory: Assets\n  ScriptModulePath: \"\"\n";
    ASSERT_TRUE(Project::Load(source / "Residency.oloproj"));
    auto editor = Ref<EditorAssetManager>::Create();
    editor->Initialize(false);
    Project::SetAssetManager(editor);
    const AssetHandle groomHandle = 15952688685437936336ull;
    const AssetHandle bindingHandle = 15952688685437936337ull;
    const AssetMetadata groomMetadata(groomHandle, AssetType::Groom, "Assets/Grooms/Dog/Dog.ologroom");
    const AssetMetadata bindingMetadata(bindingHandle, AssetType::GroomBinding, "Assets/Grooms/Dog/Dog.ologroombinding");
    editor->SetMetadata(groomHandle, groomMetadata);
    editor->SetMetadata(bindingHandle, bindingMetadata);
    ASSERT_TRUE(editor->SerializeAssetRegistry());
    AssetRegistry persisted;
    ASSERT_TRUE(persisted.Deserialize(Project::GetAssetRegistryPath()));
    EXPECT_EQ(persisted.GetMetadata(groomHandle).FilePath, groomMetadata.FilePath);
    EXPECT_EQ(persisted.GetMetadata(bindingHandle).FilePath, bindingMetadata.FilePath);
    // Initialize also discovers path-backed models/textures/scenes. They are
    // staged loose for Runtime; selecting them here would construct GPU assets.
    AssetRegistry registry;
    registry.AddAsset(persisted.GetMetadata(groomHandle));
    registry.AddAsset(persisted.GetMetadata(bindingHandle));
    fs::create_directories(game / "Assets");
    AssetPackBuilder::BuildSettings settings;
    settings.m_OutputPath = game / "Assets/AssetPack.olopack";
    settings.m_IncludeScriptModule = false;
    settings.m_CompressAssets = false;
    std::atomic<f32> progress{ 0.0f };
    const auto result = AssetPackBuilder::BuildFromRegistry(registry, settings, progress);
    ASSERT_TRUE(result.m_Success) << result.m_ErrorMessage;
    ASSERT_EQ(result.m_FailedAssetCount, 0u);
    ASSERT_EQ(result.m_AssetCount, 2u);
    auto runtime = Ref<RuntimeAssetManager>::Create(false);
    ASSERT_TRUE(runtime->LoadAssetPack(settings.m_OutputPath));
    ProjectConfig config;
    config.AssetDirectory = "Assets";
    Project::NewInMemory(game, config);
    Project::SetAssetManager(runtime);
    const auto groom = runtime->GetAsset(groomHandle).As<GroomAsset>();
    const auto binding = runtime->GetAsset(bindingHandle).As<GroomBindingAsset>();
    ASSERT_TRUE(groom);
    ASSERT_TRUE(binding);
    ASSERT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(groom));
    ASSERT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(binding));
    EXPECT_EQ(groom->GetHandle(), groomHandle);
    EXPECT_EQ(binding->GetHandle(), bindingHandle);
    EXPECT_GT(groom->GetCurveCount(), 60000u);
    ASSERT_TRUE(groom->FindLodLevel(GroomRepresentation::Card));
    EXPECT_EQ(binding->GetRootCount(), groom->GetCurveCount());
    EXPECT_EQ(binding->GetSourceSignature(), GroomBindingBuilder::SignGroom(*groom));
    EXPECT_EQ(binding->GetTargetSourcePath(), "Dog"); // The cooked binding carries the authored target name.
    runtime->Shutdown();
    Project::SetAssetManager(editor);
    ASSERT_TRUE(Project::Load(source / "Residency.oloproj"));
    sizet staged = 0;
    std::vector<std::string> unresolved;
    std::string reason;
    ASSERT_TRUE(StageSceneReferencedContent({ scenePath }, source, "Assets", OLO_TEST_EDITOR_ROOT,
                                            game, staged, unresolved, reason))
        << reason;
    EXPECT_TRUE(unresolved.empty());
    fs::create_directories(game / "Scenes");
    fs::copy_file(scenePath, game / "Scenes/RuntimeDogAndFern.olo", fs::copy_options::overwrite_existing);
    fs::current_path(OLO_TEST_EDITOR_ROOT);
    ASSERT_TRUE(GameBuildPipeline::StageRuntimeContent(game, reason)) << reason;
    EXPECT_TRUE(fs::is_regular_file(game / "Assets/Models/Dog/Dog.gltf"));
    EXPECT_TRUE(fs::is_regular_file(game / "Assets/Models/Dog/Dog.bin"));
    EXPECT_TRUE(fs::is_regular_file(game / "Assets/Models/Dog/DogEyeball.gltf"));
    const auto plant = LoadFoliageStreamingPayload(game / "Assets/Models/Vegetation/fern/fern.obj");
    ASSERT_TRUE(plant);
    EXPECT_GT(plant->Indices.Num(), 6000);
    ASSERT_EQ(plant->Parts.Num(), 1);
    EXPECT_TRUE(fs::equivalent(fs::path(plant->Parts[0].AlbedoPath.ToStdString()),
                               game / "Assets/Models/Vegetation/fern/Textures/fern_fronds.png"));
    const auto albedo = DecodeFoliageStreamingAlbedo(plant->Parts[0]);
    ASSERT_TRUE(albedo);
    EXPECT_GT(albedo->Width, 2u);
    bool transparent = false;
    bool opaque = false;
    for (i32 pixel = 3; pixel < albedo->Pixels.Num(); pixel += 4)
    {
        transparent |= albedo->Pixels[pixel] < 128;
        opaque |= albedo->Pixels[pixel] >= 128;
    }
    EXPECT_TRUE(transparent);
    EXPECT_TRUE(opaque);
    std::ofstream(game / "game.manifest") << "Game:\n  Name: ResidencyStreamingObserved\n"
                                             "StartScene: Scenes/RuntimeDogAndFern.olo\nRendering:\n  Is3DMode: true\n";
    std::ofstream(output / "pack-verification.txt")
        << "Production AssetPackBuilder::BuildFromRegistry, persisted isolated registry; two runtime-loaded assets.\n"
        << "Groom: " << static_cast<u64>(groomHandle) << "; binding: " << static_cast<u64>(bindingHandle) << '\n'
        << "Curves/roots: " << groom->GetCurveCount() << "; matching root signature; cooked Card floor.\n"
        << "Body and fern dependencies staged by StageSceneReferencedContent; real transparent fern atlas decoded.\n"
        << "CPU export only: no actual Runtime launch, bound-body visual verdict or GPU counters claimed.\n";
}
