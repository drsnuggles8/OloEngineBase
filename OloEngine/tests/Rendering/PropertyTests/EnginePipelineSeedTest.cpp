#include "OloEnginePCH.h"

// OLO_TEST_LAYER: integration
// =============================================================================
// EnginePipelineSeedTest: the checked-in engine PSO seed (issue #1650).
//
// OloEditor/assets/PipelineManifest.yaml lists the complete pipeline keys the
// engine's own scenes draw on Vulkan. The editor (cwd OloEditor/) reads it as
// its shipped manifest and Build Game merges it into a package's seed, so a
// FIRST session on a machine (empty OLO_SHADER_CACHE_DIR, no machine manifest)
// requests those keys before its first frame instead of creating each at a
// hitch. These cases keep that file honest:
//
//   * it parses whole and is in its normalized form (a regeneration of an
//     unchanged seed is a zero diff);
//   * a cold session that starts from it renders what a cold editor session
//     of the Sandbox project opens (its start scene, then the #1259
//     integrated acceptance scene, each with its own render settings and the
//     project's quality tier, as the editor and runtime apply them) on
//     Forward, Forward+ and Deferred, under CSM and VSM, and no draw creates a
//     pipeline nobody requested; a failure names every missing key and the
//     script that regenerates the file;
//   * the same cold render WITHOUT the seed does create pipelines nobody
//     requested (the A/B control: the seed is what fixed them), and a seed with
//     one of its keys deleted names exactly that key (the negative control).
//
// Vulkan only: OpenGL has no pipeline objects to precache, so every case but
// the parse check skips in the GL process. The Vulkan cases run in the
// `--olo-rhi=vulkan` process (vulkan_scene_filter.txt).
//
// EnginePipelineSeedWriter.WritesTheSeed is the regeneration mode the script
// drives (OLO_PIPELINE_SEED_OUT=<file>); it renders exactly what the guard
// renders, through the same helper, so the two cannot drift apart.
// =============================================================================

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneTransition.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelinePrecache.h"
#endif

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        // The resolution is not part of a pipeline key (viewport and scissor
        // are dynamic state), so the smallest frame that runs every pass is
        // the right one: lavapipe renders this suite in CI.
        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 180;
        constexpr u32 kFramesPerCamera = 3;
        constexpr f32 kMockTime = 12.0f;

        struct PathSpec
        {
            RenderingPath Path;
            const char* Name;
        };
        constexpr PathSpec kPaths[] = { { RenderingPath::Forward, "Forward" },
                                        { RenderingPath::ForwardPlus, "ForwardPlus" },
                                        { RenderingPath::Deferred, "Deferred" } };

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        // The test binary runs from the repo root (CI's scene step from
        // OloEditor/), so the seed is loaded by its full path, never through
        // the cwd-relative kEngineSeedPath.
        // The registry EditorAssetManager::Shutdown writes into the active
        // project, which here is the real, tracked Sandbox project.
        [[nodiscard]] fs::path RegistryPath()
        {
            return EditorRoot() / "SandboxProject" / "AssetRegistry.oar";
        }

        [[nodiscard]] fs::path SeedPath()
        {
            return EditorRoot() / fs::path(PipelineManifest::kEngineSeedPath);
        }

        [[nodiscard]] std::optional<std::string> ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return std::nullopt;
            }
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        [[nodiscard]] std::string WithoutCarriageReturns(std::string text)
        {
            std::erase(text, '\r');
            return text;
        }

        [[nodiscard]] std::string RegenerateHint()
        {
            return std::format("Regenerate the seed with `{}` (it renders the same scene on a cold Vulkan "
                               "context and rewrites {}), then review and commit the diff.",
                               PipelineManifest::kSeedRegenerationCommand, SeedPath().generic_string());
        }

        [[nodiscard]] std::string DescribeAll(const TArray<PipelineManifestEntry>& entries)
        {
            std::string text;
            for (const PipelineManifestEntry& entry : entries)
            {
                text += "\n    " + PipelineManifest::Describe(entry);
            }
            return text;
        }
    } // namespace

    class EnginePipelineSeedTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

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
            m_PreviousPath = Renderer3D::GetRendererSettings().Path;
        }

        void TearDown() override
        {
#if OLO_WITH_VULKAN
            // The precache is process-wide, and the cases after this one in the
            // Vulkan process assume what a test VulkanContext starts with: an
            // empty manifest (a test never loads a shipped one). Leaving the
            // seed, or the negative control's reduced seed, would queue its
            // requests for every shader a later case builds.
            if (m_RestartedCold)
                (void)VulkanPipelinePrecache::Get().RestartColdForTesting(PipelineManifest{});
#endif
            Renderer3D::GetShadowMap().SetSettings(m_PreviousShadow);
            Renderer3D::GetRendererSettings().Path = m_PreviousPath;
            Renderer3D::ApplyRendererSettings();
            Time::ClearMockTime();
            m_Scene = nullptr;
            m_StartScene = nullptr;
            RendererAttachedTest::TearDown();
            if (m_AssetManager)
            {
                m_AssetManager->Shutdown();
                m_AssetManager = nullptr;
            }
            // Shutdown wrote the asset manager's registry into the real
            // Sandbox project; a test must leave tracked files as it found them.
            if (m_SavedRegistry.has_value())
            {
                std::ofstream registry(RegistryPath(), std::ios::binary | std::ios::trunc);
                registry << *m_SavedRegistry;
                EXPECT_TRUE(registry.good()) << "could not restore " << RegistryPath().string();
                m_SavedRegistry.reset();
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

        // A committed Sandbox scene, deserialized from a temp copy (its
        // .scenebin sidecar must not land in the working tree).
        void LoadScene(const fs::path& relative, Ref<Scene>& scene)
        {
            const fs::path source = EditorRoot() / "SandboxProject" / "Assets" / relative;
            const fs::path copy = TempDir("pipeline-seed") / relative.filename();
            std::error_code ec;
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "cannot copy " << source.string() << ": " << ec.message();
            scene = Scene::Create();
            SceneSerializer serializer(scene);
            ASSERT_TRUE(serializer.Deserialize(copy)) << relative.string() << " failed to deserialize; see the log";
            // The editor's helpers (grid, gizmos, frustums) stay at the scene's
            // defaults, which is what the editor draws.
            scene->SetIs3DModeEnabled(true);
            scene->OnViewportResize(kWidth, kHeight);
            scene->SetRenderingEnabled(true);
        }

        // What a cold editor session of the Sandbox project opens: its start
        // scene (DriftMenu, a UI menu: the Renderer2D pipelines), then the
        // #1259 integrated acceptance scene, as AcceptanceSceneEvidenceTest
        // loads it.
        void Load()
        {
            std::error_code ec;
            fs::current_path(EditorRoot(), ec);
            ASSERT_FALSE(ec) << "cannot enter the editor root: " << ec.message();
            Project::Unload();
            ASSERT_TRUE(Project::Load(EditorRoot() / "SandboxProject" / "Sandbox.oloproj"));
            m_SavedRegistry = ReadText(RegistryPath());
            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
            Time::SetMockTime(kMockTime);

            const fs::path startScene = Project::GetActive()->GetConfig().StartScene;
            ASSERT_FALSE(startScene.empty()) << "the Sandbox project names no start scene";
            ASSERT_NO_FATAL_FAILURE(LoadScene(startScene, m_StartScene));
            ASSERT_NO_FATAL_FAILURE(LoadScene(fs::path("Scenes") / "Benchmark" / "Acceptance.olo", m_Scene));

            std::string error;
            m_Cameras = Benchmark::LoadBenchmarkManifest(
                EditorRoot() / "assets" / "benchmark" / "manifests" / "acceptance-forward.yaml", error);
            ASSERT_TRUE(m_Cameras.has_value()) << error;
            ASSERT_FALSE(m_Cameras->Cameras.IsEmpty());
        }

        [[nodiscard]] static EditorCamera Camera(const glm::vec3& position, f32 yawDegrees, f32 pitchDegrees,
                                                 f32 fovDegrees)
        {
            EditorCamera camera(fovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 600.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, glm::radians(yawDegrees), glm::radians(pitchDegrees));
            return camera;
        }

#if OLO_WITH_VULKAN
        struct ColdRun
        {
            PipelineManifest Drawn;           ///< Every key a draw used in this run.
            PipelineRequestCounters Counters; ///< The builder's, since the cold restart.
            std::string Tag;                  ///< The configuration the precache selected.
        };

        // Each scene exactly as the editor (and a packaged runtime) opens it:
        // its own render settings (post-process, wind, fog, path) and the
        // project's quality tier on top, through the call both make, then the
        // path under test. A bare fixture renders with struct defaults
        // instead, which turn off GTAO, bloom, FXAA, the depth-normal prepass
        // and wind: a seed recorded that way missed 15 keys in a cold editor
        // session.
        static void OpenAsTheEditorDoes(const Scene& scene, const PathSpec& path)
        {
            SceneTransition::ApplySceneRenderSettings(scene);
            Renderer3D::GetRendererSettings().Path = path.Path;
            Renderer3D::ApplyRendererSettings();
        }

        // A first session in this process: no created pipeline, the request
        // counters at zero and `seed` as the only manifest (requested for every
        // live shader now, and for every shader built later when it is built).
        // Then, on one path, the start scene, and the acceptance scene under
        // CSM and then VSM from every gate camera.
        [[nodiscard]] ColdRun RenderCold(const PipelineManifest& seed, const PathSpec& path)
        {
            auto& precache = VulkanPipelinePrecache::Get();
            const u64 session = precache.RestartColdForTesting(seed);
            m_RestartedCold = true;

            // The start scene on its AUTHORED path, which is how the editor
            // opens it at start-up (a UI menu: its Renderer2D keys have the
            // same targets on every path). Forced onto Deferred, this UI-only
            // scene trips #1653's WRITE_AFTER_WRITE hazard on every frame.
            SceneTransition::ApplySceneRenderSettings(*m_StartScene);
            // Under the shadow mode a session starts with (VSM is off by
            // default), not whatever the previous path's loop left behind.
            auto startShadow = Renderer3D::GetShadowMap().GetSettings();
            startShadow.VSM.Enabled = false;
            Renderer3D::GetShadowMap().SetSettings(startShadow);
            RunEditorFramesOn(*m_StartScene, Camera(glm::vec3(0.0f, 0.0f, 10.0f), 0.0f, 0.0f, 60.0f), kFramesPerCamera);

            OpenAsTheEditorDoes(*m_Scene, path);
            for (const bool vsm : { false, true })
            {
                auto shadow = Renderer3D::GetShadowMap().GetSettings();
                shadow.VSM.Enabled = vsm;
                Renderer3D::GetShadowMap().SetSettings(shadow);
                for (const auto& spec : m_Cameras->Cameras)
                {
                    const auto pose = Benchmark::CameraPoseAtFrame(spec, 0u, m_Cameras->FixedDtSeconds);
                    const EditorCamera camera = Camera(pose.Position, pose.YawDegrees, pose.PitchDegrees, spec.FovDegrees);
                    for (u32 frame = 0; frame < kFramesPerCamera; ++frame)
                    {
                        m_Scene->AdvanceAnimalPaths(Timestep{ m_Cameras->FixedDtSeconds });
                        RunEditorFramesOn(*m_Scene, camera, 1);
                    }
                }
            }

            ColdRun run;
            run.Drawn = precache.CopyManifest().SeenInSession(session);
            run.Counters = VulkanPipelineBuilder::Get().GetRequestStats().Counters;
            run.Tag = precache.GetConfigurationTag();
            std::printf("[pipeline-seed] %s: tag '%s', %zu key(s) drawn, seed %zu key(s): never-requested %llu, "
                        "too-late queued %llu, too-late in flight %llu, prepared first uses %llu, blocking %.1f ms "
                        "(max %.1f ms)\n",
                        path.Name, run.Tag.c_str(), run.Drawn.GetEntryCount(), seed.GetEntryCount(),
                        static_cast<unsigned long long>(run.Counters.NeverRequested),
                        static_cast<unsigned long long>(run.Counters.TooLateQueued),
                        static_cast<unsigned long long>(run.Counters.TooLateInFlight),
                        static_cast<unsigned long long>(run.Counters.PreparedFirstUses), run.Counters.BlockingCreateMs,
                        run.Counters.BlockingCreateMaxMs);
            return run;
        }
#endif

        Ref<Scene> m_Scene;
        Ref<Scene> m_StartScene;
        std::optional<Benchmark::BenchmarkManifest> m_Cameras;

      private:
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssetManager;
        Ref<EditorAssetManager> m_AssetManager;
        std::optional<std::string> m_SavedRegistry; ///< The project's registry bytes before the asset manager ran.
        fs::path m_PreviousCwd;
        ShadowSettings m_PreviousShadow{};
        RenderingPath m_PreviousPath = RenderingPath::Forward;
        bool m_RestartedCold = false;
    };

    // A seed that does not parse whole would be dropped at start-up (logged as
    // an error, but every first-session hitch would be back), and a seed not in
    // its normalized form makes every regeneration a noisy diff.
    TEST_F(EnginePipelineSeedTest, TheCheckedInSeedParsesWholeAndIsNormalized)
    {
        const auto text = ReadText(SeedPath());
        ASSERT_TRUE(text.has_value()) << "no engine seed at " << SeedPath().string() << ". " << RegenerateHint();
        std::string error;
        sizet rejected = 0;
        const auto seed = PipelineManifest::Parse(*text, &error, &rejected);
        ASSERT_TRUE(seed.has_value()) << "the engine seed does not parse: " << error << ". " << RegenerateHint();
        EXPECT_EQ(rejected, 0u) << "the engine seed has invalid entries. " << RegenerateHint();
        EXPECT_GT(seed->GetEntryCount(), 0u);
        // Merged with a machine manifest, which Trim caps at kDefaultMaxEntries:
        // the seed must leave most of that room to the machine's own keys.
        EXPECT_LE(seed->GetEntryCount(), PipelineManifest::kDefaultMaxEntries / 4);
        EXPECT_EQ(WithoutCarriageReturns(*text), seed->NormalizedForSeed().Serialize() + "\n")
            << "the engine seed is not in its normalized form (sorted, no sessions). " << RegenerateHint();
    }

    // THE GUARD. A cold session that starts from the checked-in seed draws
    // nothing nobody requested, on every raster path.
    TEST_F(EnginePipelineSeedTest, AColdSessionFromTheSeedRequestsEveryPipelineTheAcceptanceSceneDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TestRenderBackend::IsVulkanProcess())
            GTEST_SKIP() << "OpenGL has no pipeline objects to precache; this runs in the --olo-rhi=vulkan process";
#if OLO_WITH_VULKAN
        const auto text = ReadText(SeedPath());
        ASSERT_TRUE(text.has_value()) << RegenerateHint();
        const auto seed = PipelineManifest::Parse(*text);
        ASSERT_TRUE(seed.has_value()) << RegenerateHint();
        ASSERT_NO_FATAL_FAILURE(Load());

        PipelineManifest drawnEverywhere;
        for (const PathSpec& path : kPaths)
        {
            SCOPED_TRACE(path.Name);
            const ColdRun run = RenderCold(*seed, path);
            EXPECT_GT(run.Drawn.GetEntryCount(), 0u) << "nothing was drawn: the guard would pass on an empty frame";
            const TArray<PipelineManifestEntry> missing = run.Drawn.EntriesMissingFrom(*seed);
            EXPECT_TRUE(missing.IsEmpty()) << path.Name << " drew " << missing.Num()
                                           << " pipeline key(s) the engine seed does not request:"
                                           << DescribeAll(missing) << "\n"
                                           << RegenerateHint();
            // The counter is the acceptance number: a key the seed lists but a
            // request derived differently from the draw (a layout or blend
            // mismatch) is still a never-requested miss, and is caught here.
            EXPECT_EQ(run.Counters.NeverRequested, 0u)
                << path.Name << ": " << run.Counters.NeverRequested << " draw(s) created a pipeline nobody requested"
                << (missing.IsEmpty() ? " although every key drawn is in the seed: the request and the draw derived "
                                        "different keys (see the [Vulkan/PSO] lines in the log)"
                                      : "")
                << ". " << RegenerateHint();
            drawnEverywhere.Merge(run.Drawn);
        }
        // Informational: seed entries no path drew here (another device's
        // optional feature, or a key the scene stopped drawing). They cost a
        // start-up request each, never a hitch.
        std::printf("[pipeline-seed] seed keys not drawn by any path here: %d of %zu\n",
                    seed->EntriesMissingFrom(drawnEverywhere).Num(), seed->GetEntryCount());
#else
        GTEST_SKIP() << "built without Vulkan";
#endif
    }

    // The A/B control and the negative control, on Forward. Without the seed
    // the same cold render creates pipelines nobody requested (so the guard's
    // zero is the seed's doing); with one drawn key deleted from the seed, the
    // guard's comparison names exactly that key.
    TEST_F(EnginePipelineSeedTest, WithoutTheSeedOrWithAKeyDeletedTheColdRenderNamesWhatWasNeverRequested)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TestRenderBackend::IsVulkanProcess())
            GTEST_SKIP() << "OpenGL has no pipeline objects to precache; this runs in the --olo-rhi=vulkan process";
#if OLO_WITH_VULKAN
        const auto text = ReadText(SeedPath());
        ASSERT_TRUE(text.has_value()) << RegenerateHint();
        const auto seed = PipelineManifest::Parse(*text);
        ASSERT_TRUE(seed.has_value()) << RegenerateHint();
        ASSERT_NO_FATAL_FAILURE(Load());
        const PathSpec& forward = kPaths[0];

        const ColdRun unseeded = RenderCold(PipelineManifest{}, forward);
        EXPECT_GT(unseeded.Counters.NeverRequested, 0u) << "without a seed a cold session must miss";

        // Delete one key this render drew from a copy of the seed. Not just any
        // key: the boot-time bakes (BRDF LUT, IBL convolution, impostor bake)
        // run once per scene load, so a second render never draws them again.
        // The tone map runs every frame on every path.
        const TArray<PipelineManifestEntry> drawn = unseeded.Drawn.EntriesMissingFrom(PipelineManifest{});
        const PipelineManifestEntry* victimPtr = drawn.FindByPredicate([](const PipelineManifestEntry& e)
                                                                       { return e.Shader.ToView() == "PostProcess_ToneMap"; });
        ASSERT_NE(victimPtr, nullptr) << "Forward drew no tone-map pipeline:" << DescribeAll(drawn);
        const PipelineManifestEntry victim = *victimPtr;
        PipelineManifest reduced = *seed;
        ASSERT_TRUE(reduced.Remove(victim.Shader.ToView(), victim.StageMask, victim.Signature))
            << "the seed does not hold a key Forward draws: " << PipelineManifest::Describe(victim) << ". "
            << RegenerateHint();

        const ColdRun partial = RenderCold(reduced, forward);
        const TArray<PipelineManifestEntry> missing = partial.Drawn.EntriesMissingFrom(reduced);
        ASSERT_EQ(missing.Num(), 1) << "expected exactly the deleted key:" << DescribeAll(missing);
        EXPECT_EQ(missing[0].Shader.ToView(), victim.Shader.ToView());
        EXPECT_EQ(missing[0].StageMask, victim.StageMask);
        EXPECT_TRUE(missing[0].Signature == victim.Signature);
        EXPECT_GE(partial.Counters.NeverRequested, 1u) << "the deleted key's draw is a never-requested miss";
        std::printf("[pipeline-seed] negative control: deleted %s; named %s\n",
                    PipelineManifest::Describe(victim).c_str(), PipelineManifest::Describe(missing[0]).c_str());
#else
        GTEST_SKIP() << "built without Vulkan";
#endif
    }

    // REGENERATION MODE (scripts/regenerate-pipeline-seed.py). Not in the CI
    // filter: it writes a file instead of checking one.
    class EnginePipelineSeedWriter : public EnginePipelineSeedTest
    {
    };

    TEST_F(EnginePipelineSeedWriter, WritesTheSeed)
    {
        const char* out = std::getenv("OLO_PIPELINE_SEED_OUT");
        if (out == nullptr || *out == '\0')
            GTEST_SKIP() << "set OLO_PIPELINE_SEED_OUT=<file> (" << PipelineManifest::kSeedRegenerationCommand
                         << " does) to regenerate the engine seed";
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TestRenderBackend::IsVulkanProcess())
            GTEST_FAIL() << "the seed is recorded on Vulkan: run with --olo-rhi=vulkan";
#if OLO_WITH_VULKAN
        ASSERT_NO_FATAL_FAILURE(Load());
        PipelineManifest seed;
        for (const PathSpec& path : kPaths)
        {
            const ColdRun run = RenderCold(PipelineManifest{}, path);
            ASSERT_GT(run.Drawn.GetEntryCount(), 0u) << path.Name << " drew nothing";
            seed.Merge(run.Drawn);
        }
        const std::string text = seed.NormalizedForSeed().Serialize() + "\n";
        std::ofstream file(fs::path(out), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file) << "cannot write " << out;
        file << text;
        file.close();
        ASSERT_TRUE(file) << "cannot write " << out;
        std::printf("[pipeline-seed] wrote %zu key(s) to %s\n", seed.GetEntryCount(), out);
#else
        GTEST_FAIL() << "built without Vulkan";
#endif
    }
} // namespace OloEngine::Tests
