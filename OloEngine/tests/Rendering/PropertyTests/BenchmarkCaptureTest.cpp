// =============================================================================
// BenchmarkCaptureTest.cpp
//
// The deterministic benchmark-capture entry point for issue #974, gated behind
// `--olo-capture-manifest=<path>` exactly the way ShaderPackBakeTest gates the
// shader-pack bake: a capture is a TOOL RUN that writes a result directory as
// a side effect, not a correctness check that should run every suite
// invocation. (The test main() narrows the gtest filter to this case whenever
// the flag is present, so one flag gives a pure capture run.)
//
// What a run does — the contract docs/guides/renderer-benchmarks.md pins:
//   parse + validate the manifest → mount the real SandboxProject → seed the
//   RNG and freeze the mock clock at StartTimeSeconds → deserialize the scene
//   → apply the manifest's renderer-side settings + exposure → warm N frames
//   per camera, STEPPING the mock clock by FixedDtSeconds each frame (every
//   Time::GetTime()-derived clock advances identically run to run) → capture
//   every declared attachment at native resolution → write the
//   self-describing result directory (files + manifest echo + result.json).
//
// Determinism model: a capture is a fresh process rendering a fixed number of
// frames, so the free-running frame counters (stochastic index, TAA jitter,
// fog/cloud indices, DDGI frame index) hold identical values at the capture
// frame without reset plumbing. The proof is running the tool twice and
// diffing within the manifest's Tolerance.RepeatRmse.
//
// OLO_TEST_LAYER: plumbing
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include "RenderPropertyTest.h"
#include "TestOptions.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace OloEngine;

namespace
{
    // The GpuContext init switches the process CWD to OloEditor/, but the
    // manifest path on the command line was typed against the INVOKING shell's
    // CWD (usually the repo root). Try it as given, then repo-root-relative.
    fs::path ResolveManifestPath(const std::string& given)
    {
        const fs::path direct(given);
        if (fs::exists(direct))
        {
            return direct;
        }
        if (const fs::path fromRepoRoot = fs::path("..") / given; fs::exists(fromRepoRoot))
        {
            return fromRepoRoot;
        }
        return {};
    }

    struct ScopedMockClock
    {
        explicit ScopedMockClock(f32 startSeconds)
        {
            Time::SetMockTime(startSeconds);
        }
        ~ScopedMockClock()
        {
            Time::ClearMockTime();
        }
        ScopedMockClock(const ScopedMockClock&) = delete;
        auto operator=(const ScopedMockClock&) -> ScopedMockClock& = delete;
    };
} // namespace

TEST(BenchmarkCapture, RunWhenRequested)
{
    const auto& opts = OloEngine::Tests::Options();
    if (opts.CaptureManifestPath.empty())
    {
        GTEST_SKIP() << "pass --olo-capture-manifest=<path> to run a benchmark capture "
                        "(the issue-#974 entry point; see docs/guides/renderer-benchmarks.md)";
    }

    // An explicitly REQUESTED capture must not skip-and-exit-0 on a headless
    // box — the caller asked for a result directory, so "no GPU" is a failure
    // of the invocation, not an environment to tolerate the way ordinary
    // suite runs do.
    if (!::OloEngine::Tests::RenderPropertyFixture::IsGpuAvailable())
    {
        FAIL() << "--olo-capture-manifest was given but no usable GL 4.6 context is available — "
                  "a capture invocation cannot succeed headless (run on a GPU box, or use the "
                  "editor front door)";
    }

    // -- Manifest (parsed BEFORE renderer init: the mock clock must be live
    //    when Renderer3D seeds its dt trackers, or the first frame's fog/wind
    //    dt depends on process uptime — a fixed phase offset in every
    //    accumulated history that run-twice comparisons would not see but
    //    cross-invocation-context comparisons would) ------------------------
    const fs::path manifestPath = ResolveManifestPath(opts.CaptureManifestPath);
    ASSERT_FALSE(manifestPath.empty()) << "manifest not found: tried '" << opts.CaptureManifestPath << "' and '"
                                       << (fs::path("..") / opts.CaptureManifestPath).string()
                                       << "' (cwd is OloEditor/ once the GL context is up)";
    std::string parseError;
    const auto manifest = Benchmark::LoadBenchmarkManifest(manifestPath, parseError);
    ASSERT_TRUE(manifest.has_value()) << parseError;

    if (!manifest->SupportsBackend("opengl"))
    {
        GTEST_SKIP() << "manifest '" << manifest->Id
                     << "' does not declare the opengl backend; run it through the editor front door";
    }

    // -- Determinism levers, BEFORE the renderer and the scene exist --------
    RandomUtils::SetGlobalSeed(manifest->Seed);
    ScopedMockClock mockClock(manifest->StartTimeSeconds);

    if (!Renderer3D::IsInitialized())
    {
        Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
    }

    // -- Project mount (the REAL SandboxProject — capture reads assets, and
    //    the .scenebin sidecar Deserialize writes next to the scene is
    //    git-ignored) --------------------------------------------------------
    const fs::path projectFile = fs::path("SandboxProject") / "Sandbox.oloproj";
    ASSERT_TRUE(fs::exists(projectFile)) << "expected to run from OloEditor/ (cwd: " << fs::current_path().string()
                                         << ")";
    ASSERT_TRUE(Project::Load(projectFile));
    auto assetManager = Ref<EditorAssetManager>::Create();
    // No file watcher: a capture only reads.
    assetManager->Initialize(/*startFileWatcher=*/false);
    Project::SetAssetManager(assetManager);
    struct AssetManagerShutdown
    {
        Ref<EditorAssetManager> Mgr;
        ~AssetManagerShutdown()
        {
            if (Mgr)
            {
                Mgr->Shutdown();
            }
        }
    } assetManagerShutdown{ assetManager };

    // -- Scene --------------------------------------------------------------
    const fs::path scenePath = Project::GetAssetDirectory() / manifest->ScenePath;
    ASSERT_TRUE(fs::exists(scenePath)) << "manifest Scene not found: " << scenePath.string();
    auto scene = Scene::Create();
    SceneSerializer serializer(scene);
    ASSERT_TRUE(serializer.Deserialize(scenePath)) << "scene failed to deserialize — see the engine log";

    // A benchmark capture is a picture of the SCENE, not of the editor: turn
    // off every editor-only viewport helper the editor render path would
    // otherwise draw into the frame (the infinite grid, the world-axis helper,
    // light gizmos, camera frustums).
    scene->SetGridVisible(false);
    scene->SetWorldAxisHelperVisible(false);
    scene->SetLightGizmosVisible(false);
    scene->SetCameraFrustumsVisible(false);

    // -- Renderer-side settings the scene cannot serialize ------------------
    // Snapshot everything this run overwrites — the manifest-applied structs
    // AND the seven scene-copied blocks — so a user-filtered invocation that
    // runs other GPU tests after this one (`--gtest_filter=*` overrides the
    // capture-mode narrowing) does not leak benchmark-scene fog/wind/snow
    // settings into them.
    const RendererSettings savedRendererSettings = Renderer3D::GetRendererSettings();
    const PostProcessSettings savedPostProcessSettings = Renderer3D::GetPostProcessSettings();
    const auto savedSnowSettings = Renderer3D::GetSnowSettings();
    const auto savedWindSettings = Renderer3D::GetWindSettings();
    const auto savedSnowAccumulationSettings = Renderer3D::GetSnowAccumulationSettings();
    const auto savedSnowEjectaSettings = Renderer3D::GetSnowEjectaSettings();
    const auto savedPrecipitationSettings = Renderer3D::GetPrecipitationSettings();
    const auto savedFogSettings = Renderer3D::GetFogSettings();
    {
        // The editor's scene-open copies the scene's serialized settings
        // blocks into the renderer (EditorLayer's OpenScene finalizer). Plain
        // Deserialize only STORES them on the Scene — without these copies the
        // scene's SSAO/fog/wind flags silently never reach the GPU and their
        // AOVs (AOBuffer, FogColor) have no backing at capture time. Mirror
        // the editor's full set; quality tiering is deliberately NOT applied —
        // the manifest pins quality explicitly.
        Renderer3D::GetPostProcessSettings() = scene->GetPostProcessSettings();
        Renderer3D::GetSnowSettings() = scene->GetSnowSettings();
        Renderer3D::GetWindSettings() = scene->GetWindSettings();
        Renderer3D::GetSnowAccumulationSettings() = scene->GetSnowAccumulationSettings();
        Renderer3D::GetSnowEjectaSettings() = scene->GetSnowEjectaSettings();
        Renderer3D::GetPrecipitationSettings() = scene->GetPrecipitationSettings();
        Renderer3D::GetFogSettings() = scene->GetFogSettings();

        // The manifest's renderer-side state, applied ON TOP of the scene's
        // blocks — ONE shared implementation with the editor front door.
        Benchmark::ApplyManifestRendererState(*manifest);
    }

    // -- Enable the full 3D draw path (same recipe as RendererAttachedTest::
    //    EnableRendering: both resizes are required or the graph runs against
    //    unsized framebuffers) ----------------------------------------------
    const u32 width = manifest->Width;
    const u32 height = manifest->Height;
    scene->SetIs3DModeEnabled(true);
    scene->OnViewportResize(width, height);
    Renderer3D::OnWindowResize(width, height);
    scene->SetRenderingEnabled(true);

    // -- Warm + capture, per camera, one monotonic mock clock ---------------
    const f32 dt = manifest->FixedDtSeconds;
    const Timestep ts{ dt };
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    u32 frameIndex = 0;
    std::vector<Benchmark::CameraCaptureSet> cameraSets;

    for (const auto& cameraSpec : manifest->Cameras)
    {
        EditorCamera camera(cameraSpec.FovDegrees, aspect, cameraSpec.NearClip, cameraSpec.FarClip);
        camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
        camera.SetPose(cameraSpec.Position, glm::radians(cameraSpec.YawDegrees),
                       glm::radians(cameraSpec.PitchDegrees));

        const u32 warmFrames = cameraSpec.WarmupFrames.value_or(manifest->WarmupFrames);
        for (u32 i = 0; i < warmFrames; ++i)
        {
            Time::SetMockTime(manifest->StartTimeSeconds + static_cast<f32>(frameIndex) * dt);
            {
                GLStateGuard guard("BenchmarkCapture", GLStateGuard::Policy::Restore);
                scene->OnUpdateEditor(ts, camera);
            }
            ++frameIndex;
        }

        const Benchmark::CaptureContext captureContext{ cameraSpec.NearClip, cameraSpec.FarClip };
        auto set = Benchmark::CaptureCameraSet(*manifest, cameraSpec.Id, frameIndex, "opengl", captureContext);
        for (const auto& attachment : set.Attachments)
        {
            EXPECT_TRUE(attachment.Error.empty()) << "camera '" << cameraSpec.Id << "' attachment '"
                                                  << attachment.Spec.Name << "': " << attachment.Error;
        }
        cameraSets.push_back(std::move(set));
    }

    // -- Provenance + result directory --------------------------------------
    Benchmark::RunInfo runInfo;
    runInfo.Backend = "opengl";
    if (const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR)))
    {
        runInfo.GpuVendor = vendor;
    }
    if (const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
    {
        runInfo.GpuRenderer = renderer;
    }
    runInfo.CommitSha = Benchmark::QueryCommitShaViaGit();
    runInfo.MachineTag = Benchmark::ResolveMachineTag(opts.PerfMachine);
    runInfo.Host = "test-binary";
    runInfo.TotalFramesRendered = frameIndex;
    // The clock value the LAST RENDERED frame actually saw — the loop sets
    // the mock BEFORE incrementing frameIndex, so it is (frameIndex - 1) * dt.
    // Anyone reconstructing the captured frame's animation phase needs this
    // exact value, not one tick ahead of it. (The schema guarantees at least
    // one warm frame, so frameIndex >= 1 here.)
    runInfo.FinalMockTimeSeconds = manifest->StartTimeSeconds + static_cast<f32>(frameIndex - 1) * dt;
    runInfo.PassTimings = Benchmark::SnapshotPassTimings();
    runInfo.Counters = Benchmark::SnapshotRendererCounters();

    const fs::path outDir = !opts.CaptureOutDir.empty()
                                ? fs::path(opts.CaptureOutDir)
                                : fs::path("assets") / "benchmark" / "captures" / manifest->Id;
    std::string writeError;
    ASSERT_TRUE(Benchmark::WriteResultDirectory(*manifest, manifestPath, outDir, cameraSets, runInfo, writeError))
        << writeError;

    // -- Restore the process-wide renderer configuration (every block this
    //    run overwrote, not just the two structs — see the snapshot above) ---
    Renderer3D::GetPostProcessSettings() = savedPostProcessSettings;
    Renderer3D::GetRendererSettings() = savedRendererSettings;
    Renderer3D::GetSnowSettings() = savedSnowSettings;
    Renderer3D::GetWindSettings() = savedWindSettings;
    Renderer3D::GetSnowAccumulationSettings() = savedSnowAccumulationSettings;
    Renderer3D::GetSnowEjectaSettings() = savedSnowEjectaSettings;
    Renderer3D::GetPrecipitationSettings() = savedPrecipitationSettings;
    Renderer3D::GetFogSettings() = savedFogSettings;
    Renderer3D::ApplyRendererSettings();
    Renderer3D::SetRenderScale(1.0f);

    OLO_CORE_INFO("[BenchmarkCapture] '{}' captured {} camera(s) over {} frame(s) into '{}'", manifest->Id,
                  cameraSets.size(), frameIndex, outDir.string());
}
