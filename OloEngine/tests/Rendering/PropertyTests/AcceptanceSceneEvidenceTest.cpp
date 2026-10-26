#include "OloEnginePCH.h"

// OLO_TEST_LAYER: integration
// =============================================================================
// AcceptanceSceneEvidenceTest — the #1259 integrated acceptance scene.
//
// `Scenes/Benchmark/Acceptance.olo` is generated (scripts/perf/
// generate-acceptance-scene.py) by splicing committed scenes together, and the
// manifests beside it pose cameras over a procedural terrain whose heights the
// generator cannot compute. Each contract below is a way that splice can be
// wrong while every individual source scene is right:
//
//   * a coat bound to one body but parented to another (or to none) still
//     renders, just in the wrong place or at the full cost of a far root
//     (docs/agent-rules/a-bound-coat-rides-its-body.md);
//   * a walking animal off the terrain, or not following it, floats or sinks;
//   * more foliage-interaction sources than the field's slots only evict each
//     other;
//   * a shadowed light that does not request the ray-traced tier makes the
//     hybrid cell a copy of Deferred;
//   * a camera path that dips under a hill captures the inside of the terrain;
//   * a camera that does not see its subject captures nothing worth gating.
//   * 44 bound coats built in the scene's first frame take the load gigabytes
//     past what the scene holds afterwards (#1591).
//
// The last is the visual evidence: every manifest camera on every raster path,
// written as Acceptance_GL_<Path>_<Camera>.png under assets/tests/visual/,
// beside Acceptance_GL_MidLoad.png and Acceptance_GL_Loaded.png.
// Needs a GL 4.6 context; skips cleanly without one.
// =============================================================================

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/TerrainGround.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr f32 kMockTime = 12.0f;
        constexpr f32 kMinCameraClearance = 1.0f; // metres above the ground along every camera path
        constexpr f32 kTerrainMargin = 2.0f;      // metres a path figure keeps from the terrain's edge

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] std::vector<fs::path> AcceptanceManifests()
        {
            std::vector<fs::path> paths;
            for (const auto& entry : fs::directory_iterator(EditorRoot() / "assets" / "benchmark" / "manifests"))
            {
                const std::string name = entry.path().filename().string();
                if (name.starts_with("acceptance-") && entry.path().extension() == ".yaml")
                    paths.push_back(entry.path());
            }
            return paths;
        }
    } // namespace

    class AcceptanceSceneTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;
            EnableRendering(kWidth, kHeight);
        }

        void SetUp() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssetManager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            RendererAttachedTest::SetUp();
            std::error_code ec;
            m_PreviousCwd = fs::current_path(ec);
            m_PreviousShadow = Renderer3D::GetShadowMap().GetSettings();
        }

        void TearDown() override
        {
            Renderer3D::GetShadowMap().SetSettings(m_PreviousShadow);
            Time::ClearMockTime();
            m_Scene = nullptr;
            RendererAttachedTest::TearDown();
            if (m_AssetManager)
            {
                m_AssetManager->Shutdown();
                m_AssetManager = nullptr;
            }
            std::error_code ec;
            if (!m_PreviousCwd.empty())
                fs::current_path(m_PreviousCwd, ec);
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            else
                Project::Unload();
            if (m_PreviousAssetManager)
                Project::SetAssetManager(m_PreviousAssetManager);
        }

        // The committed scene, deserialized from a temp copy (its .scenebin
        // sidecar must not land in the working tree) in the real project, then
        // ticked until the terrain height field and the flora exist.
        void Load(u32 frames = 3u)
        {
            std::error_code ec;
            fs::current_path(EditorRoot(), ec);
            ASSERT_FALSE(ec) << "cannot enter the editor root: " << ec.message();
            Project::Unload();
            ASSERT_TRUE(Project::Load(EditorRoot() / "SandboxProject" / "Sandbox.oloproj"));
            // The scene's coats are asset handles, so the project needs an asset
            // manager of its own; a process that runs this suite alone has none.
            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
            Time::SetMockTime(kMockTime);

            const fs::path source = EditorRoot() / "SandboxProject" / "Assets" / "Scenes" / "Benchmark" / "Acceptance.olo";
            const fs::path copy = TempDir("acceptance") / "Acceptance.olo";
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "cannot copy " << source.string() << ": " << ec.message();

            m_Scene = Scene::Create();
            SceneSerializer serializer(m_Scene);
            ASSERT_TRUE(serializer.Deserialize(copy)) << "Acceptance.olo failed to deserialize; see the engine log";
            // A picture of the scene, not of the editor: no grid, axis helper,
            // light gizmos or camera frustums (the capture host does the same).
            m_Scene->SetGridVisible(false);
            m_Scene->SetWorldAxisHelperVisible(false);
            m_Scene->SetLightGizmosVisible(false);
            m_Scene->SetCameraFrustumsVisible(false);
            m_Scene->SetIs3DModeEnabled(true);
            m_Scene->OnViewportResize(kWidth, kHeight);
            m_Scene->SetRenderingEnabled(true);
            RunEditorFramesOn(*m_Scene, OverviewCamera(), frames);
        }

        [[nodiscard]] static EditorCamera OverviewCamera()
        {
            return Camera({ 128.0f, 80.0f, 300.0f }, 0.0f, -20.0f, 60.0f);
        }

        // The composited frame, rows top-down, written for inspection as
        // assets/tests/visual/<name>.png; the pixels come back for checks.
        void WriteFrame(const std::string& name, std::vector<u8>& rgba)
        {
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(rgba, w, h));
            ASSERT_EQ(rgba.size(), static_cast<sizet>(w) * h * 4u);
            const sizet rowBytes = static_cast<sizet>(w) * 4u;
            std::vector<u8> row(rowBytes);
            for (u32 y = 0; y < h / 2u; ++y)
            {
                std::memcpy(row.data(), rgba.data() + y * rowBytes, rowBytes);
                std::memcpy(rgba.data() + y * rowBytes, rgba.data() + (h - 1u - y) * rowBytes, rowBytes);
                std::memcpy(rgba.data() + (h - 1u - y) * rowBytes, row.data(), rowBytes);
            }
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            fs::create_directories(dir);
            const std::string file = (dir / (name + ".png")).string();
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << file;
        }

        [[nodiscard]] static EditorCamera Camera(const glm::vec3& position, f32 yawDegrees, f32 pitchDegrees,
                                                 f32 fovDegrees)
        {
            EditorCamera camera(fovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 600.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, glm::radians(yawDegrees), glm::radians(pitchDegrees));
            return camera;
        }

        [[nodiscard]] const TerrainComponent* Terrain() const
        {
            for (const auto entity : m_Scene->GetAllEntitiesWith<TerrainComponent>())
                return &m_Scene->GetAllEntitiesWith<TerrainComponent>().get<TerrainComponent>(entity);
            return nullptr;
        }

        Ref<Scene> m_Scene;

      private:
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssetManager;
        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_PreviousCwd;
        ShadowSettings m_PreviousShadow{};
    };

    // Every coat rides the body its binding targets; every walking animal
    // follows the ground, stays on the terrain and stands on it after walking;
    // the interaction field is not oversubscribed; every shadowed light can be
    // traced.
    TEST_F(AcceptanceSceneTest, EverySubjectIsBoundGroundedAndOnTheTerrain)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load());
        const TerrainComponent* terrain = Terrain();
        ASSERT_NE(terrain, nullptr);
        ASSERT_TRUE(terrain->m_TerrainData) << "the terrain height field was not built by the rendered ticks";

        u32 coats = 0;
        for (const auto e : m_Scene->GetAllEntitiesWith<GroomBindingComponent>())
        {
            Entity coat{ e, m_Scene.get() };
            const auto& binding = coat.GetComponent<GroomBindingComponent>();
            SCOPED_TRACE(coat.GetComponent<TagComponent>().Tag);
            const std::optional<Entity> body = m_Scene->TryGetEntityWithUUID(binding.m_TargetEntity);
            ASSERT_TRUE(body.has_value()) << "binding target missing";
            EXPECT_TRUE(body->HasComponent<MeshComponent>());
            ASSERT_TRUE(coat.HasComponent<RelationshipComponent>());
            EXPECT_EQ(static_cast<u64>(coat.GetComponent<RelationshipComponent>().m_ParentHandle),
                      static_cast<u64>(binding.m_TargetEntity))
                << "a bound coat must be parented to the body its binding targets";
            ++coats;
        }
        // Two horses, the head's hair, and every fox's coat.
        EXPECT_GE(coats, 3u);

        u32 pushers = 0;
        for (const auto e : m_Scene->GetAllEntitiesWith<FoliageInteractionComponent>())
        {
            if (m_Scene->GetAllEntitiesWith<FoliageInteractionComponent>().get<FoliageInteractionComponent>(e).m_Enabled)
                ++pushers;
        }
        EXPECT_GE(pushers, 1u);
        EXPECT_LE(pushers, kFoliageInteractionSlots) << "more interaction sources than the field has slots";

        std::vector<Entity> walkers;
        for (const auto e : m_Scene->GetAllEntitiesWith<AnimalPathComponent>())
        {
            Entity walker{ e, m_Scene.get() };
            const auto& path = walker.GetComponent<AnimalPathComponent>();
            const glm::vec3 origin = walker.GetComponent<TransformComponent>().Translation;
            SCOPED_TRACE(walker.GetComponent<TagComponent>().Tag);
            EXPECT_TRUE(path.m_FollowTerrain);
            EXPECT_GE(origin.x - path.m_RadiusX, kTerrainMargin);
            EXPECT_LE(origin.x + path.m_RadiusX, terrain->m_WorldSizeX - kTerrainMargin);
            EXPECT_GE(origin.z - path.m_RadiusZ, kTerrainMargin);
            EXPECT_LE(origin.z + path.m_RadiusZ, terrain->m_WorldSizeZ - kTerrainMargin);
            walkers.push_back(walker);
        }
        EXPECT_GE(walkers.size(), 40u) << "the population and the horses walk";

        for (u32 step = 0; step < 120; ++step)
            m_Scene->AdvanceAnimalPaths(Timestep{ 1.0f / 60.0f });
        for (Entity walker : walkers)
        {
            const glm::vec3 p = walker.GetComponent<TransformComponent>().Translation;
            const auto ground = SampleTerrainGroundHeight(*m_Scene, p.x, p.z);
            ASSERT_TRUE(ground.has_value()) << walker.GetComponent<TagComponent>().Tag << " walked off the terrain";
            EXPECT_NEAR(p.y, *ground, 0.01f) << walker.GetComponent<TagComponent>().Tag;
        }

        // The hybrid cell turns the renderer's ray-traced shadows on, which
        // traces only the lights that request it; one that does not leaves the
        // cell a copy of Deferred.
        u32 shadowedLights = 0;
        const auto expectTraced = [this, &shadowedLights]<typename TLight>(std::type_identity<TLight>)
        {
            for (const auto e : m_Scene->GetAllEntitiesWith<TLight>())
            {
                Entity entity{ e, m_Scene.get() };
                const auto& light = entity.GetComponent<TLight>();
                if (!light.m_CastShadows)
                    continue;
                ++shadowedLights;
                EXPECT_TRUE(light.m_RayTracedShadows)
                    << entity.GetComponent<TagComponent>().Tag << " casts a shadow but does not request the ray-traced tier";
            }
        };
        expectTraced(std::type_identity<DirectionalLightComponent>{});
        expectTraced(std::type_identity<PointLightComponent>{});
        expectTraced(std::type_identity<SpotLightComponent>{});
        // The sun, the head's key and side lights, and the moving lantern.
        EXPECT_GE(shadowedLights, 4u);
    }

    // The generator cannot evaluate the procedural terrain, so it places
    // cameras and the moving light from a committed height grid this test
    // exports from the real height field: OLO_ACCEPTANCE_GROUND_EXPORT=1 writes
    // scripts/perf/acceptance-ground.json (8 m grid over the terrain).
    TEST_F(AcceptanceSceneTest, ExportsTheGroundHeightGrid)
    {
        const char* flag = std::getenv("OLO_ACCEPTANCE_GROUND_EXPORT");
        if (flag == nullptr || flag[0] != '1')
            GTEST_SKIP() << "set OLO_ACCEPTANCE_GROUND_EXPORT=1 to regenerate scripts/perf/acceptance-ground.json";
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load());
        const TerrainComponent* terrain = Terrain();
        ASSERT_NE(terrain, nullptr);
        constexpr f32 kStep = 8.0f;
        const u32 columns = static_cast<u32>(terrain->m_WorldSizeX / kStep) + 1u;
        const u32 rows = static_cast<u32>(terrain->m_WorldSizeZ / kStep) + 1u;
        std::string json = "{\n  \"stepMetres\": 8,\n  \"columns\": " + std::to_string(columns) +
                           ",\n  \"rows\": " + std::to_string(rows) + ",\n  \"heights\": [";
        for (u32 row = 0; row < rows; ++row)
        {
            json += row == 0 ? "\n    [" : ",\n    [";
            for (u32 column = 0; column < columns; ++column)
            {
                const f32 x = std::min(static_cast<f32>(column) * kStep, terrain->m_WorldSizeX);
                const f32 z = std::min(static_cast<f32>(row) * kStep, terrain->m_WorldSizeZ);
                const auto ground = SampleTerrainGroundHeight(*m_Scene, x, z);
                ASSERT_TRUE(ground.has_value()) << x << ", " << z;
                char value[32];
                std::snprintf(value, sizeof(value), "%s%.3f", column == 0 ? "" : ", ", static_cast<f64>(*ground));
                json += value;
            }
            json += "]";
        }
        json += "\n  ]\n}\n";
        const fs::path out = EditorRoot().parent_path() / "scripts" / "perf" / "acceptance-ground.json";
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good()) << out.string();
        file << json;
    }

    // Every camera of every acceptance manifest stays above the ground along
    // its whole trajectory: warm-up, measured frames and any sequence.
    TEST_F(AcceptanceSceneTest, EveryManifestCameraClearsTheTerrainAlongItsPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load());
        const auto manifests = AcceptanceManifests();
        ASSERT_FALSE(manifests.empty());
        for (const auto& manifestPath : manifests)
        {
            std::string error;
            const auto manifest = Benchmark::LoadBenchmarkManifest(manifestPath, error);
            ASSERT_TRUE(manifest.has_value()) << manifestPath.string() << ": " << error;
            for (const auto& camera : manifest->Cameras)
            {
                SCOPED_TRACE(manifestPath.filename().string() + " / " + camera.Id.ToStdString());
                const u32 frames = Benchmark::CameraFrameCount(*manifest, camera);
                if (const auto ground = SampleTerrainGroundHeight(*m_Scene, camera.Position.x, camera.Position.z))
                {
                    // Recorded for whoever places the cameras in the generator.
                    RecordProperty(camera.Id.ToStdString() + "_start_ground", std::to_string(*ground));
                }
                f32 lowest = 1.0e9f;
                for (u32 frame = 0; frame < frames; frame += 4u)
                {
                    const auto pose = Benchmark::CameraPoseAtFrame(camera, frame, manifest->FixedDtSeconds);
                    if (const auto ground = SampleTerrainGroundHeight(*m_Scene, pose.Position.x, pose.Position.z))
                        lowest = std::min(lowest, pose.Position.y - *ground);
                }
                if (lowest > 1.0e8f)
                    continue; // never over the terrain (the head studio stands beside it)
                std::printf("[acceptance] %s %s: lowest clearance %.2f m\n", manifestPath.filename().string().c_str(),
                            camera.Id.ToStdString().c_str(), static_cast<f64>(lowest));
                EXPECT_GE(lowest, kMinCameraClearance);
            }
        }
    }

    // Each camera of the gate manifest, on each raster path with virtual
    // shadow maps on, written for inspection. The check here only refuses an
    // empty or flat frame; what the subject looks like is judged from the PNG.
    TEST_F(AcceptanceSceneTest, EveryGateCameraSeesItsSubjectOnEveryRasterPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load());
        std::string error;
        const auto manifest = Benchmark::LoadBenchmarkManifest(
            EditorRoot() / "assets" / "benchmark" / "manifests" / "acceptance-deferred.yaml", error);
        ASSERT_TRUE(manifest.has_value()) << error;

        auto shadow = Renderer3D::GetShadowMap().GetSettings();
        shadow.VSM.Enabled = true;
        Renderer3D::GetShadowMap().SetSettings(shadow);

        const struct
        {
            RenderingPath Path;
            const char* Name;
        } paths[] = { { RenderingPath::Forward, "Forward" },
                      { RenderingPath::ForwardPlus, "ForwardPlus" },
                      { RenderingPath::Deferred, "Deferred" } };
        for (const auto& path : paths)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path.Path;
            Renderer3D::ApplyRendererSettings();
            for (const auto& spec : manifest->Cameras)
            {
                SCOPED_TRACE(std::string(path.Name) + " / " + spec.Id.ToStdString());
                const u32 warm = spec.WarmupFrames.value_or(manifest->WarmupFrames);
                const auto pose = Benchmark::CameraPoseAtFrame(spec, warm, manifest->FixedDtSeconds);
                // Walk the animals as the capture step does (an edit-mode tick
                // never runs the "AnimalPaths" system), one step per frame.
                const EditorCamera camera = Camera(pose.Position, pose.YawDegrees, pose.PitchDegrees, spec.FovDegrees);
                for (u32 frame = 0; frame < 24u; ++frame)
                {
                    m_Scene->AdvanceAnimalPaths(Timestep{ manifest->FixedDtSeconds });
                    RunEditorFramesOn(*m_Scene, camera, 1);
                }

                std::vector<u8> rgba;
                ASSERT_NO_FATAL_FAILURE(
                    WriteFrame("Acceptance_GL_" + std::string(path.Name) + "_" + spec.Id.ToStdString(), rgba));
                f64 sum = 0.0;
                f64 sumSq = 0.0;
                for (sizet i = 0; i < rgba.size(); i += 4u)
                {
                    const f64 luma = 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                    sum += luma;
                    sumSq += luma * luma;
                }
                const f64 n = static_cast<f64>(rgba.size() / 4u);
                const f64 mean = sum / n;
                const f64 stddev = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
                EXPECT_GT(mean, 8.0) << "the frame is black";
                EXPECT_GT(stddev, 6.0) << "the frame is flat: no subject in view";
            }
        }
    }

    // #1591: the scene's bound coats arrive over its first frames, a few a
    // frame, inside the strand cache's budget. Built all at once -- every coat
    // at full detail, the LOD's hold not yet run out -- they held 3.6 GiB
    // against a 1 GiB budget and took the process past 9 GB resident, over
    // the 6 GB this host allows. The first frame and the loaded one are
    // written for inspection: a coat not built yet is absent, never drawn
    // wrong, and the pass counts it.
    TEST_F(AcceptanceSceneTest, BoundCoatsArriveOverTheFirstFramesWithinTheBudget)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // A cold load: an earlier case's coats share these entity ids, and the
        // cache would hand them straight back.
        GroomRenderPass* pass = Renderer3D::GetGroomRenderPass();
        ASSERT_NE(pass, nullptr);
        pass->ReleaseCachedGeometry();
        ASSERT_NO_FATAL_FAILURE(Load(1u));
        // From the camera that frames the herd: a coat no view can reach is
        // never requested, so the overview the load warms up with sees none.
        std::string error;
        const auto manifest = Benchmark::LoadBenchmarkManifest(
            EditorRoot() / "assets" / "benchmark" / "manifests" / "acceptance-deferred.yaml", error);
        ASSERT_TRUE(manifest.has_value()) << error;
        const Benchmark::ManifestCamera* herdSpec = nullptr;
        for (const auto& spec : manifest->Cameras)
        {
            if (spec.Id.ToStdString() == "population-dolly")
                herdSpec = &spec;
        }
        ASSERT_NE(herdSpec, nullptr) << "the gate manifest has no population-dolly camera";
        const auto herdPose = Benchmark::CameraPoseAtFrame(*herdSpec, 0u, manifest->FixedDtSeconds);
        const EditorCamera herd = Camera(herdPose.Position, herdPose.YawDegrees, herdPose.PitchDegrees, herdSpec->FovDegrees);
        // The coats reach the pass once their assets have loaded, which takes
        // seconds; the first frame that submits them is the one judged.
        u32 frames = 1u;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (pass->GetStats().GroomsSubmitted == 0u && std::chrono::steady_clock::now() < deadline)
        {
            RunEditorFramesOn(*m_Scene, herd, 1);
            ++frames;
        }
        ASSERT_GT(pass->GetStats().GroomsSubmitted, 0u) << "no coat reached the groom pass in " << frames << " frames";
        frames = 1u;

        const GroomRenderStats first = pass->GetStats();
        EXPECT_GT(first.GroomsAwaitingBuild, 0u) << "every coat was built in the scene's first frame";
        EXPECT_EQ(first.CacheOverBudgetBytes, 0u);
        std::vector<u8> rgba;
        ASSERT_NO_FATAL_FAILURE(WriteFrame("Acceptance_GL_MidLoad", rgba));

        u64 peakCached = first.CachedBytes;
        while (pass->GetStats().GroomsAwaitingBuild > 0u && frames < 64u)
        {
            RunEditorFramesOn(*m_Scene, herd, 1);
            ++frames;
            peakCached = std::max(peakCached, pass->GetStats().CachedBytes);
            EXPECT_EQ(pass->GetStats().CacheOverBudgetBytes, 0u) << "frame " << frames;
        }
        const GroomRenderStats loaded = pass->GetStats();
        EXPECT_EQ(loaded.GroomsAwaitingBuild, 0u) << "coats still waiting after " << frames << " frames";
        EXPECT_GT(loaded.GroomsGpuDeformed, first.GroomsGpuDeformed);
        ASSERT_NO_FATAL_FAILURE(WriteFrame("Acceptance_GL_Loaded", rgba));
        std::printf("[acceptance-load] %u coats waited at the first frame; all built after %u frames; strand cache "
                    "peak %.0f of %.0f MiB\n",
                    first.GroomsAwaitingBuild, frames, static_cast<f64>(peakCached) / (1024.0 * 1024.0),
                    static_cast<f64>(loaded.CacheBudgetBytes) / (1024.0 * 1024.0));
    }
} // namespace OloEngine::Tests
