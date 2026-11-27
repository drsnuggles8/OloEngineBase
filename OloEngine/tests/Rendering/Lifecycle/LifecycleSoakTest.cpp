// OLO_TEST_LAYER: integration
// =============================================================================
// The lifecycle soak (issue #1796): one long-lived process walks real user
// workflows over two production scenes, again and again, on the real renderer,
// and checks that nothing accumulates and nothing stale survives.
//
// The sequence, per cycle (each step followed by a few rendered frames):
//
//   open A, play, stop, spawn entities, destroy them, resize, resize back,
//   quality tier Low, Ultra, path Forward, Deferred, Forward+, shader reload,
//   open B, open A again  -> compare A with A's control, sample memory
//
// and once at the end: unload, renderer Shutdown, census, Init, open A
// -> compare A with A's control again.
//
// Two hosts drive it, the two ways a user reaches a scene:
//
//   * The editor (loose assets): EditorLayer's open (scene-swap reset, then
//     the scene's authored settings with the project tier on top), Play on a
//     Scene::Copy and Stop back to the authored scene, frames through the
//     editor camera.
//   * The runtime (cooked assets): OloRuntime's ActivateScene, step for step,
//     over the game directory GameBuildPipeline::StageRuntimeContent lays out,
//     with a RuntimeAssetManager; frames through the scene's primary camera.
//     The runtime has no play/stop: its scene runs from activation to the next.
//
// A is SandboxProject's ForwardPlusTest (authored Forward+), B is
// ContactShadowTest (authored Deferred), so every return to A has to put the
// path, the post stack and the tier back. The negative control
// (MissingActivationResetIsCaught) turns that reset off with the
// FaultSkipSceneRenderSettingsPublish lever and requires the return-to-A
// comparison to fail.
//
// What the per-PR smoke checks (the defaults: one warm-up cycle, two measured):
// A after every return matches A's control within the control's own noise
// floor; GPU resident memory after returning to A does not grow across the
// measured cycles; the restarted renderer's A matches too; no Vulkan validation
// error (the fixture's tick already fails on one). Process resident memory is
// recorded and judged with a looser tolerance, because the allocator and the
// driver keep pages a process never gives back.
//
// The long soak is scripts/lifecycle-soak.ps1: more cycles, a JSON report
// (--olo-lifecycle-soak-report) and a fresh-process control
// (--olo-lifecycle-soak-control-out / -in). docs/testing/lifecycle-soak.md has
// the coverage manifest and the owner.
//
// What it substitutes (substituted-seams-compound.md): no window, no ImGui, no
// present; the editor's panels are not involved, so a tier change is applied
// as RendererSettingsPanel::ApplyQualityTieringToRuntime applies it, and a path
// change as the renderer settings panel writes it. OloRuntime's RuntimeLayer is
// compiled into the executable only, so its ActivateScene is mirrored here call
// for call (OloRuntimeApp.cpp) rather than called.
// =============================================================================

#include "OloEnginePCH.h"

#include "LifecycleSoak.h"
#include "MemoryCeiling.h"
#include "PropertyTests/RendererAttachedTest.h"
#include "PropertyTests/RenderPropertyTest.h"
#include "ScopedFileRestore.h"
#include "TestOptions.h"
#include "PropertyTests/TestRenderBackend.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Core/BuildInfo.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneTransition.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanDevice.h"
#endif

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace OloEngine::Tests::Lifecycle
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr u32 kWidth = 480;
        constexpr u32 kHeight = 270;
        constexpr u32 kAltWidth = 640;
        constexpr u32 kAltHeight = 360;
        constexpr f32 kDt = 1.0f / 60.0f;
        // Frames after each step, and after opening the scene a comparison
        // reads. Enough for every temporal history the scenes use to converge;
        // the control floor measures whatever is left.
        constexpr u32 kStepFrames = 3;
        constexpr u32 kSettleFrames = 12;
        constexpr u32 kSpawnedEntities = 8;
        constexpr u64 kMiB = 1024ull * 1024ull;
        // GPU memory is the renderer's own ledger and returns exactly to where a
        // warm cache leaves it; 1 MiB is a few small buffers of slack, not a
        // render target (one 480x270 RGBA16F target is ~1 MiB).
        constexpr u64 kGpuGrowthTolerance = 1 * kMiB;
        // The process figure includes the heap, the driver and the shader
        // cache; a measured cycle that keeps 64 MiB is a leak worth a look.
        constexpr u64 kProcessGrowthTolerance = 64 * kMiB;

        constexpr const char* kSceneA = "ForwardPlusTest.olo";
        constexpr const char* kSceneB = "ContactShadowTest.olo";

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] fs::path SandboxProjectFile()
        {
            return EditorRoot() / "SandboxProject" / "Sandbox.oloproj";
        }

        [[nodiscard]] fs::path RegistryPath()
        {
            return EditorRoot() / "SandboxProject" / "AssetRegistry.oar";
        }

        [[nodiscard]] fs::path LooseScene(const char* name)
        {
            return EditorRoot() / "SandboxProject" / "Assets" / "Scenes" / name;
        }

        // The editor camera a user lands on: the scene's own primary camera
        // pose. Both scenes look straight down -Z, so the rotation is a pitch.
        [[nodiscard]] std::optional<EditorCamera> CameraFor(Scene& scene, std::string& error)
        {
            Entity primary = scene.GetPrimaryCameraEntity();
            if (!primary)
            {
                error = "the scene has no primary camera";
                return std::nullopt;
            }
            const auto& transform = primary.GetComponent<TransformComponent>();
            const glm::vec3 euler = transform.GetRotationEuler();
            if (std::abs(euler.y) > 1e-4f || std::abs(euler.z) > 1e-4f)
            {
                error = "the scene's camera has yaw or roll; CameraFor only derives a pitch";
                return std::nullopt;
            }
            const auto& camera = primary.GetComponent<CameraComponent>();
            EditorCamera editor(glm::degrees(camera.Camera.GetPerspectiveVerticalFOV()),
                                static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                camera.Camera.GetPerspectiveNearClip(), camera.Camera.GetPerspectiveFarClip());
            editor.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // EditorCamera's positive pitch tilts the view down; a transform's
            // negative X rotation does.
            editor.SetPose(transform.Translation, 0.0f, -euler.x);
            return editor;
        }

        // <stem>_<leg>_<backend><ext>: one report (or control frame) per leg and
        // backend, so the legs and the two backends' processes never overwrite
        // each other's. The editor and the runtime draw A through different
        // cameras, so each leg needs its own fresh-process control.
        [[nodiscard]] std::string ReportPath(const std::string& base, const std::string& leg)
        {
            if (base.empty())
                return {};
            const fs::path path(base);
            const std::string backend = TestRenderBackend::IsVulkanProcess() ? "Vulkan" : "GL";
            return (path.parent_path() / (path.stem().string() + "_" + leg + "_" + backend + path.extension().string()))
                .string();
        }

        // <Feature>_<Backend>_<Path>_<What>.png under assets/tests/visual/
        // (git-ignored), for a reviewer to look at: a comparison that holds
        // on two empty frames is the failure this exists to catch.
        void WriteEvidence(const std::string& feature, const std::string& what, const Frame& frame)
        {
            if (frame.Width == 0u || frame.Height == 0u)
                return;
            // ReadbackComposite returns rows bottom-first on both backends.
            const sizet rowBytes = static_cast<sizet>(frame.Width) * 4u;
            std::vector<u8> topDown(frame.Rgba.size());
            for (u32 y = 0; y < frame.Height; ++y)
                std::memcpy(topDown.data() + y * rowBytes, frame.Rgba.data() + (frame.Height - 1u - y) * rowBytes,
                            rowBytes);
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string file = (dir / (TestRenderBackend::EvidenceName(feature) + what + ".png")).string();
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(frame.Width), static_cast<int>(frame.Height), 4,
                                       topDown.data(), static_cast<int>(rowBytes)),
                      0)
                << file;
        }

        [[nodiscard]] u64 ValidationErrors()
        {
#if OLO_WITH_VULKAN
            if (TestRenderBackend::IsVulkanProcess())
                return VulkanDevice::GetValidationErrorCount();
#endif
            return 0;
        }
    } // namespace

    enum class Host : u8
    {
        Editor,
        Runtime
    };

    class LifecycleSoak : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
        }

        void SetUp() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssetManager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            RendererAttachedTest::SetUp();
            if (IsSkipped() || HasFatalFailure())
                return;
            // After the base SetUp, which enters OloEditor/: that is the
            // directory every later test resolves its shaders against, and the
            // one TearDown puts back.
            std::error_code ec;
            m_PreviousCwd = fs::current_path(ec);
            m_PreviousShadow = Renderer3D::GetShadowMap().GetSettings();
            m_SavedFault = Levers::FaultSkipSceneRenderSettingsPublish();
            // What Application's startup does before any scene runs. Lua's Init
            // is once per process and safe to repeat. C# is not started: Mono
            // cannot start again in a process once shut down, and neither scene
            // carries a script (ScriptEngine tolerates the absent engine).
            LuaScriptEngine::Init();
            m_ValidationAtStart = ValidationErrors();
            m_SetUpComplete = true;
        }

        void TearDown() override
        {
            if (m_Running && GetSceneRef())
                GetSceneRef()->OnRuntimeStop();
            m_Running = false;
            m_EditScene = nullptr;
            // Only what SetUp saved is put back. The scene stays set: the base
            // TearDown resets the frame camera only for a fixture with a scene,
            // and the next test must not cull against this one's camera.
            if (m_SetUpComplete)
            {
                Levers::SetFaultSkipSceneRenderSettingsPublish(m_SavedFault);
                Renderer3D::GetShadowMap().SetSettings(m_PreviousShadow);
            }
            RendererAttachedTest::TearDown();

            if (m_EditorAssetManager)
            {
                m_EditorAssetManager->Shutdown();
                m_EditorAssetManager = nullptr;
            }
            m_RuntimeAssetManager = nullptr;
            // EditorAssetManager::Shutdown wrote the registry into the real
            // Sandbox project; a test leaves tracked files as it found them.
            if (m_Registry.has_value())
            {
                EXPECT_TRUE(m_Registry->Restore()) << "could not restore " << m_Registry->GetPath().string();
                m_Registry.reset();
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

        // --- Contexts ------------------------------------------------------------

        // The editor: the real project, its asset manager, the editor root as
        // the working directory.
        void EnterEditor()
        {
            std::error_code ec;
            fs::current_path(EditorRoot(), ec);
            ASSERT_FALSE(ec) << "cannot enter the editor root: " << ec.message();
            Project::Unload();
            ASSERT_TRUE(Project::Load(SandboxProjectFile())) << SandboxProjectFile().string();
            m_Registry.emplace(RegistryPath());
            if (!m_Registry->IsCaptured())
            {
                m_Registry.reset();
                FAIL() << "cannot read " << RegistryPath().string() << " whole, so it could not be put back";
            }
            m_EditorAssetManager = Ref<EditorAssetManager>::Create();
            m_EditorAssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_EditorAssetManager);
        }

        // OloRuntime's MountGameProject: an in-memory project at the game
        // directory, which is also the working directory, and a
        // RuntimeAssetManager. The directory is laid out once per process by
        // the build pipeline's own staging, from the editor's context.
        void EnterCookedRuntime()
        {
            static fs::path s_Staged;
            if (s_Staged.empty() || !fs::exists(s_Staged / "Scenes"))
            {
                ASSERT_NO_FATAL_FAILURE(EnterEditor());
                const fs::path root = TempRoot() / "LifecycleSoakGame";
                std::error_code ec;
                fs::remove_all(root, ec);
                fs::create_directories(root, ec);
                ASSERT_FALSE(ec) << ec.message();
                std::string error;
                ASSERT_TRUE(GameBuildPipeline::StageRuntimeContent(root, error)) << error;
                s_Staged = root;
                // Back out of the editor context before mounting the game.
                m_EditorAssetManager->Shutdown();
                m_EditorAssetManager = nullptr;
                ASSERT_TRUE(m_Registry->Restore());
                m_Registry.reset();
            }
            m_GameRoot = s_Staged;

            ProjectConfig config;
            config.Name = "LifecycleSoakGame";
            config.AssetDirectory = "Assets";
            Project::NewInMemory(m_GameRoot, config);
            m_RuntimeAssetManager = Ref<RuntimeAssetManager>::Create(/*autoLoadDefaultPack=*/false);
            Project::SetAssetManager(m_RuntimeAssetManager);
            std::error_code ec;
            fs::current_path(m_GameRoot, ec);
            ASSERT_FALSE(ec) << "cannot enter the game directory: " << ec.message();
        }

        // CopySceneFiles keeps the asset-relative path under Scenes/, so the
        // shipped copy of Assets/Scenes/<name> is Scenes/Scenes/<name>.
        [[nodiscard]] fs::path ScenePath(const char* name) const
        {
            if (m_Host == Host::Runtime)
                return m_GameRoot / "Scenes" / "Scenes" / name;
            return LooseScene(name);
        }

        // --- The two hosts' scene activation ---------------------------------------

        void Prepare(Scene& scene) const
        {
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);
            scene.SetCameraFrustumsVisible(false);
            scene.SetIs3DModeEnabled(true);
            scene.OnViewportResize(RenderWidth(), RenderHeight());
            scene.SetRenderingEnabled(true);
        }

        // EditorLayer::OpenScene -> LoadEditorSceneFile: deserialize into a new
        // scene, SetEditorScene (whose ResetRendererForSceneSwap drops the
        // renderer's scene state and republishes the live settings), then the
        // scene's authored settings with the project's tier on top.
        void EditorOpen(const char* name)
        {
            // From a temp copy: the serializer writes a .scenebin sidecar next
            // to the file it reads, which must not land in the working tree.
            const fs::path copy = TempDir("scenes") / name;
            std::error_code ec;
            fs::copy_file(ScenePath(name), copy, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "cannot copy " << ScenePath(name).string() << ": " << ec.message();

            auto loaded = SceneTransition::LoadSceneFile(copy, /*requirePrimaryCamera=*/true);
            ASSERT_TRUE(loaded) << loaded.Error.ToView();
            if (m_Running && GetSceneRef())
            {
                GetSceneRef()->OnRuntimeStop();
                m_Running = false;
            }
            m_EditScene = loaded.LoadedScene;
            SetScene(m_EditScene);
            ResetRendererForSceneSwap();
            SceneTransition::ApplySceneRenderSettings(*m_EditScene);
            Prepare(*m_EditScene);
            std::string error;
            m_Camera = CameraFor(*m_EditScene, error);
            ASSERT_TRUE(m_Camera.has_value()) << name << ": " << error;
        }

        // OloRuntime's ActivateScene, call for call: load and validate first,
        // stop the outgoing scene, then authored settings + tier, viewport,
        // start.
        void RuntimeActivate(const char* name)
        {
            auto loaded = SceneTransition::LoadSceneFile(ScenePath(name), /*requirePrimaryCamera=*/true);
            ASSERT_TRUE(loaded) << loaded.Error.ToView();
            if (m_Running && GetSceneRef())
                GetSceneRef()->OnRuntimeStop();
            m_Running = false;
            SetScene(loaded.LoadedScene);
            GetSceneRef()->SetIs3DModeEnabled(true);
            SceneTransition::ApplySceneRenderSettings(GetScene());
            Prepare(GetScene());
            GetSceneRef()->OnRuntimeStart();
            m_Running = true;
        }

        void Open(const char* name)
        {
            if (m_Host == Host::Editor)
                EditorOpen(name);
            else
                RuntimeActivate(name);
        }

        // The editor's ResetRendererForSceneSwap: drop the renderer's scene
        // state, keep the live settings.
        static void ResetRendererForSceneSwap()
        {
            const auto live = SceneTransition::SceneRenderSettings::CaptureRenderer();
            Renderer3D::ResetSceneState();
            live.PublishToRenderer();
        }

        // EditorLayer::OnScenePlay + StartActiveRuntimeScene: remember the
        // renderer's settings for Stop, copy the authored scene, give the copy
        // the live world settings, the scene-swap reset, the user's
        // render-interpolation preference (on by default), start.
        void EditorPlay()
        {
            m_RenderSettingsAtPlay = SceneTransition::SceneRenderSettings::CaptureRenderer();
            m_ShadowSettingsAtPlay = Renderer3D::GetShadowMap().GetSettings();
            m_RendererSettingsAtPlay = Renderer3D::GetRendererSettings();
            SetScene(Scene::Copy(m_EditScene));
            SceneTransition::CaptureRendererWorldSettings(GetScene());
            ResetRendererForSceneSwap();
            Prepare(GetScene());
            GetSceneRef()->SetRenderInterpolationEnabled(true);
            GetSceneRef()->OnRuntimeStart();
            m_Running = true;
        }

        // EditorLayer::OnSceneStop: stop the copy, back to the authored scene,
        // the scene-swap reset, and nothing Play wrote to the renderer outlives
        // it (#1563).
        void EditorStop()
        {
            GetSceneRef()->OnRuntimeStop();
            m_Running = false;
            SetScene(m_EditScene);
            ResetRendererForSceneSwap();
            // EditorLayer's m_RestoreRenderSettingsOnStop: only what Play saved.
            if (m_RenderSettingsAtPlay && m_ShadowSettingsAtPlay && m_RendererSettingsAtPlay)
            {
                m_RenderSettingsAtPlay->PublishToRenderer();
                Renderer3D::GetShadowMap().SetSettings(*m_ShadowSettingsAtPlay);
                Renderer3D::GetRendererSettings() = *m_RendererSettingsAtPlay;
                Renderer3D::ApplyRendererSettings();
            }
            m_RenderSettingsAtPlay.reset();
            m_ShadowSettingsAtPlay.reset();
            m_RendererSettingsAtPlay.reset();
        }

        void Unload()
        {
            if (m_Running && GetSceneRef())
                GetSceneRef()->OnRuntimeStop();
            m_Running = false;
            m_EditScene = nullptr;
            SetScene(Scene::Create());
            Renderer3D::ResetSceneState();
            Prepare(GetScene());
        }

        // --- Frames and measurements ------------------------------------------------

        // One frame through whichever path the host draws with, timed.
        void RenderFrame()
        {
            const auto start = std::chrono::steady_clock::now();
            if (m_Running || m_Host == Host::Runtime)
                RunFrames(1, kDt);
            else
                RunEditorFramesOn(GetScene(), *m_Camera, 1, kDt);
            const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
            m_CycleTimes.Push(ms);
            m_AllTimes.Push(ms);
        }

        void Frames(u32 count)
        {
            for (u32 i = 0; i < count && !HasFatalFailure(); ++i)
                RenderFrame();
        }

        [[nodiscard]] Lifecycle::Frame Capture()
        {
            Lifecycle::Frame frame;
            EXPECT_TRUE(ReadbackComposite(frame.Rgba, frame.Width, frame.Height));
            return frame;
        }

        // Retire everything a fence still holds, so "resident" is what the
        // renderer keeps, not what it has not yet been allowed to free.
        static void DrainDeletion()
        {
            auto& frames = FrameResourceManager::Get();
            for (u32 i = 0; i < FrameResourceManager::NUM_BUFFERED_FRAMES; ++i)
                frames.WaitForFrame(i);
            frames.FlushAllDeletionQueues();
        }

        void Step(const std::string& op, const std::function<void()>& action)
        {
            if (HasFatalFailure())
                return;
            action();
            if (HasFatalFailure())
                return;
            Frames(kStepFrames);
            ++m_Executed[op];
            if (m_RecordOps)
                m_Report.Operations.push_back(op);
        }

        void SetTier(QualityPreset preset)
        {
            // RendererSettingsPanel::ApplyQualityTieringToRuntime.
            const QualityTieringSettings qt = GetPresetSettings(preset);
            ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
            ApplyTieringToSettings(qt, Renderer3D::GetPostProcessSettings(), shadow);
            Renderer3D::GetShadowMap().SetSettings(shadow);
            ApplyTieringToRendererSettings(qt, Renderer3D::GetRendererSettings());
            Renderer3D::ApplyRendererSettings();
        }

        // ShaderLibrary::ReloadShaders keeps a shader's previous program when its
        // reload fails, so the frame cannot show a failure; the library's own
        // compile_finished record does. Only the shader library's record is
        // read: a script compile records the same category.
        static void ReloadShadersChecked()
        {
            auto& log = DiagnosticsEventLog::Get();
            DiagnosticEventQuery query;
            query.MaxCount = 1;
            query.Categories = { DiagnosticEventCategory::CompileFinished };
            const auto before = log.Query(query);
            query.SinceId = before.empty() ? 0 : before.back().Id;
            query.MaxCount = 64;
            Renderer3D::GetShaderLibrary().ReloadShaders();
            const auto after = log.Query(query);
            const auto shaderRecord = std::ranges::find_if(after, [](const DiagnosticEvent& e)
                                                           { return e.Message.starts_with("shader compile"); });
            ASSERT_NE(shaderRecord, after.end()) << "ShaderLibrary::ReloadShaders recorded no shader compile_finished";
            EXPECT_NE(shaderRecord->Message.find("succeeded"), std::string::npos)
                << "a shader kept its previous program: " << shaderRecord->Message;
        }

        static void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        void Resize(u32 width, u32 height)
        {
            ResizeRenderTarget(width, height);
            if (m_Camera)
                m_Camera->SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
        }

        void SpawnEntities()
        {
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            ASSERT_TRUE(cube);
            for (u32 i = 0; i < kSpawnedEntities; ++i)
            {
                Entity e = GetSceneRef()->CreateEntity("LifecycleSoakSpawn" + std::to_string(i));
                e.GetComponent<TransformComponent>().Translation = { -3.5f + static_cast<f32>(i), 0.5f, 2.0f };
                auto& mesh = e.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Cube;
                mesh.m_MeshSource = cube->GetMeshSource();
                e.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.9f, 0.2f, 0.2f, 1.0f });
                m_Spawned.push_back(e.GetUUID());
            }
        }

        void DestroySpawned()
        {
            for (const UUID id : m_Spawned)
            {
                if (auto e = GetSceneRef()->TryGetEntityWithUUID(id))
                    GetSceneRef()->DestroyEntity(*e);
            }
            m_Spawned.clear();
        }

        // A settled frame of A, its noise floor (the same state a few frames
        // later), and the frame index it was read at.
        struct Settled
        {
            Lifecycle::Frame Image;
            FrameDiff Floor;
        };

        [[nodiscard]] Settled SettleAndCapture()
        {
            Settled settled;
            Frames(kSettleFrames);
            settled.Image = Capture();
            Frames(kStepFrames);
            settled.Floor = CompareFrames(settled.Image, Capture());
            return settled;
        }

        // A's state as the renderer holds it right after activation: what the
        // negative control's comparison reads beside the image.
        struct Metadata
        {
            RenderingPath Path = RenderingPath::Forward;
            bool Bloom = false;
            bool Fxaa = false;
            u32 ShadowResolution = 0;
            auto operator==(const Metadata&) const -> bool = default;
            [[nodiscard]] std::string Describe() const
            {
                return std::string("path ") + std::string(ToToken(Path)) + ", bloom " + (Bloom ? "on" : "off") +
                       ", fxaa " + (Fxaa ? "on" : "off") + ", shadow " + std::to_string(ShadowResolution);
            }
        };

        [[nodiscard]] static Metadata ReadMetadata()
        {
            Metadata m;
            m.Path = Renderer3D::GetRendererSettings().Path;
            m.Bloom = Renderer3D::GetPostProcessSettings().BloomEnabled;
            m.Fxaa = Renderer3D::GetPostProcessSettings().FXAAEnabled;
            m.ShadowResolution = Renderer3D::GetShadowMap().GetSettings().Resolution;
            return m;
        }

        // --- The sequence --------------------------------------------------------

        // One cycle, from A to A. Leaves A open, settled, and returns how A
        // compares with `control`.
        [[nodiscard]] FrameVerdict Cycle(const Settled& control)
        {
            m_Spawned.clear();
            if (m_Host == Host::Editor)
            {
                Step("play", [this]
                     { EditorPlay(); });
                Step("stop", [this]
                     { EditorStop(); });
            }
            Step("spawn entities", [this]
                 { SpawnEntities(); });
            Step("destroy entities", [this]
                 { DestroySpawned(); });
            Step("resize", [this]
                 { Resize(kAltWidth, kAltHeight); });
            Step("resize back", [this]
                 { Resize(kWidth, kHeight); });
            Step("tier low", [this]
                 { SetTier(QualityPreset::Low); });
            Step("tier ultra", [this]
                 { SetTier(QualityPreset::Ultra); });
            Step("path forward", []
                 { SetPath(RenderingPath::Forward); });
            Step("path deferred", []
                 { SetPath(RenderingPath::Deferred); });
            Step("path forward+", []
                 { SetPath(RenderingPath::ForwardPlus); });
            Step("shader reload", []
                 { ReloadShadersChecked(); });
            Step("open B", [this]
                 { Open(kSceneB); });
            if (m_RecordOps && !HasFatalFailure())
                WriteEvidence(EvidenceFeature(), "Deferred_B", Capture());
            Step("open A", [this]
                 { Open(kSceneA); });
            m_RecordOps = false;
            if (HasFatalFailure())
                return {};
            const Settled returned = SettleAndCapture();
            return JudgeAgainstControl(CompareFrames(control.Image, returned.Image), control.Floor);
        }

        [[nodiscard]] std::string EvidenceFeature() const
        {
            return m_Host == Host::Editor ? "LifecycleSoakEditor" : "LifecycleSoakRuntime";
        }

        [[nodiscard]] std::string LegName() const
        {
            return m_Host == Host::Editor ? "editor-loose" : "runtime-cooked";
        }

        void RunSoak(Host host)
        {
            m_Host = host;
            const TestOptions& options = Options();
            if (host == Host::Editor)
                ASSERT_NO_FATAL_FAILURE(EnterEditor());
            else
                ASSERT_NO_FATAL_FAILURE(EnterCookedRuntime());

            m_Report.Leg = LegName();
            m_Report.Backend = TestRenderBackend::IsVulkanProcess() ? "Vulkan" : "GL"; // as in the file names
            m_Report.Build = std::string(BuildInfo::GetGitDescribe()) + " (" + BuildInfo::GetGitHash() + ")";
            m_Report.WarmUpCycles = options.LifecycleSoakWarmUp;
            m_Report.Cycles = options.LifecycleSoakCycles;
            m_Report.Skipped = {
                "msaa: not a production preset change; the state-machine harness owns MSAA transitions (#1349)",
                "upscale: not a production preset change; the state-machine harness owns upscale transitions",
            };
            if (host == Host::Runtime)
                m_Report.Skipped.emplace_back("play/stop: the runtime has no edit mode; its scene runs from activation");

            // A's control: the first time this process opens it.
            ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
            m_Report.Operations.emplace_back("open A");
            ++m_Executed["open A"];
            const Settled control = SettleAndCapture();
            ASSERT_FALSE(HasFatalFailure());
            const Metadata controlMetadata = ReadMetadata();
            WriteEvidence(EvidenceFeature(), "ForwardPlus_AControl", control.Image);
            EXPECT_EQ(controlMetadata.Path, RenderingPath::ForwardPlus)
                << "A is authored on Forward+; the activation did not publish it";

            if (!options.LifecycleSoakControlOut.empty())
            {
                const std::string out = ReportPath(options.LifecycleSoakControlOut, LegName());
                ASSERT_TRUE(SaveFrame(out, control.Image)) << out;
                std::cout << "[LifecycleSoak] wrote the fresh-process control to " << out << '\n';
                return;
            }

            ASSERT_GT(RendererMemoryTracker::GetInstance().GetGpuResidentBytes(), 0u)
                << "the renderer's memory ledger is empty: nothing below could see a leak";

            std::vector<u64> gpuSamples;
            std::vector<u64> processSamples;
            const u32 total = options.LifecycleSoakWarmUp + options.LifecycleSoakCycles;
            for (u32 cycle = 0; cycle < total && !HasFatalFailure(); ++cycle)
            {
                m_CycleTimes.Reset();
                m_RecordOps = cycle == 0;
                const FrameVerdict verdict = Cycle(control);
                if (HasFatalFailure())
                    break;
                const bool warmUp = cycle < options.LifecycleSoakWarmUp;
                if (cycle + 1u == total)
                    WriteEvidence(EvidenceFeature(), "ForwardPlus_AReturned", Capture());
                EXPECT_TRUE(verdict.Held) << LegName() << " cycle " << cycle << ": A after returning from B is not A: "
                                          << verdict.Describe();
                EXPECT_EQ(ReadMetadata(), controlMetadata)
                    << "cycle " << cycle << ": A returned as " << ReadMetadata().Describe() << ", control "
                    << controlMetadata.Describe();

                CycleRecord record;
                record.Cycle = cycle;
                record.WarmUp = warmUp;
                record.FrameIndexAtReturn = Renderer3D::GetPhysicalFrameSerial();
                record.GpuRetiringBytes = RendererMemoryTracker::GetInstance().GetGpuRetiringBytes();
                DrainDeletion();
                record.GpuResidentBytes = RendererMemoryTracker::GetInstance().GetGpuResidentBytes();
                record.ProcessResidentBytes = CurrentResidentBytes();
                record.ReturnToA = verdict.Diff;
                record.FrameTimes = m_CycleTimes.Query();
                m_Report.Records.push_back(record);
                if (!warmUp)
                {
                    gpuSamples.push_back(record.GpuResidentBytes);
                    processSamples.push_back(record.ProcessResidentBytes);
                }
            }
            ASSERT_FALSE(HasFatalFailure());

            m_Report.GpuGrowth = JudgeGrowth(gpuSamples, kGpuGrowthTolerance);
            m_Report.ProcessGrowth = JudgeGrowth(processSamples, kProcessGrowthTolerance);
            EXPECT_TRUE(m_Report.GpuGrowth->Bounded) << m_Report.GpuGrowth->Describe("GPU resident after returning to A");
            EXPECT_TRUE(m_Report.ProcessGrowth->Bounded)
                << m_Report.ProcessGrowth->Describe("process resident after returning to A");

            if (!options.LifecycleSoakControlIn.empty())
            {
                const std::string in = ReportPath(options.LifecycleSoakControlIn, LegName());
                const auto fresh = LoadFrame(in);
                ASSERT_TRUE(fresh.has_value()) << "cannot read the fresh-process control " << in;
                const Settled now = SettleAndCapture();
                const FrameVerdict verdict = JudgeAgainstControl(CompareFrames(*fresh, now.Image), control.Floor);
                m_Report.FreshProcessControl = verdict.Diff;
                EXPECT_TRUE(verdict.Held) << "A after the soak against a fresh process's A: " << verdict.Describe();
            }

            // Unload, shut the renderer down, bring it back, open A again.
            m_RecordOps = true;
            Step("unload", [this]
                 { Unload(); });
            Renderer3D::Shutdown();
            const auto census = Renderer3D::DebugOwnedStateCensus();
            EXPECT_EQ(census.ViewHistoryFacts, 0u) << "Shutdown left camera history for the next renderer";
            EXPECT_EQ(census.ScenePoseRecords, 0u) << "Shutdown left object pose records";
            EXPECT_EQ(census.FrameHandOffs, 0u) << "Shutdown left a pending hand-off";
            EXPECT_TRUE(Renderer3D::DebugLiveGpuOwningStatics().empty())
                << "Shutdown left GPU-owning statics alive";
            ++m_Executed["shutdown"];
            m_Report.Operations.emplace_back("shutdown");
            Renderer3D::Init();
            ASSERT_TRUE(Renderer3D::HasInitialized());
            ++m_Executed["init"];
            m_Report.Operations.emplace_back("init");
            EnableRendering(kWidth, kHeight);
            ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
            ++m_Executed["open A after restart"];
            m_Report.Operations.emplace_back("open A after restart");
            const Settled restarted = SettleAndCapture();
            const FrameVerdict afterRestart = JudgeAgainstControl(CompareFrames(control.Image, restarted.Image), control.Floor);
            m_Report.AfterRestart = afterRestart.Diff;
            WriteEvidence(EvidenceFeature(), "ForwardPlus_AAfterRestart", restarted.Image);
            EXPECT_TRUE(afterRestart.Held) << "A on the restarted renderer is not A: " << afterRestart.Describe();
            EXPECT_EQ(ReadMetadata(), controlMetadata) << "after restart: " << ReadMetadata().Describe();

            m_Report.ValidationDiagnostics = static_cast<u32>(ValidationErrors() - m_ValidationAtStart);
            EXPECT_EQ(m_Report.ValidationDiagnostics, 0u) << "Vulkan validation errors during the soak";
            m_Report.AllFrameTimes = m_AllTimes.Query();
            for (const auto& [op, count] : m_Executed)
                m_Report.Executed.emplace_back(op, count);

            WriteReport();
        }

        void WriteReport() const
        {
            std::ostringstream summary;
            const FrameTimeTailStats& t = m_Report.AllFrameTimes;
            summary << "[LifecycleSoak] " << m_Report.Leg << " on " << m_Report.Backend << ": "
                    << m_Report.WarmUpCycles << " warm-up + " << m_Report.Cycles << " cycles, " << t.SampleCount
                    << " frames, p50 " << t.P50Ms << " ms, p95 " << t.P95Ms << " ms, p99 " << t.P99Ms << " ms\n";
            for (const CycleRecord& r : m_Report.Records)
            {
                summary << "  cycle " << r.Cycle << (r.WarmUp ? " (warm-up)" : "") << ": frame " << r.FrameIndexAtReturn
                        << ", GPU " << r.GpuResidentBytes / kMiB << " MiB (retiring " << r.GpuRetiringBytes / kMiB
                        << " MiB before drain), process " << r.ProcessResidentBytes / kMiB << " MiB, A vs control "
                        << r.ReturnToA.Describe() << '\n';
            }
            if (m_Report.GpuGrowth)
                summary << "  " << m_Report.GpuGrowth->Describe("GPU") << '\n';
            if (m_Report.ProcessGrowth)
                summary << "  " << m_Report.ProcessGrowth->Describe("process") << '\n';
            if (m_Report.AfterRestart)
                summary << "  after restart: " << m_Report.AfterRestart->Describe() << '\n';
            std::cout << summary.str();

            const std::string path = ReportPath(Options().LifecycleSoakReport, m_Report.Leg);
            if (!path.empty())
            {
                std::ofstream out(path, std::ios::trunc);
                EXPECT_TRUE(out.good()) << "cannot write " << path;
                WriteJson(out, m_Report);
                std::cout << "[LifecycleSoak] report: " << path << '\n';
            }
        }

        Host m_Host = Host::Editor;
        std::optional<SceneTransition::SceneRenderSettings> m_RenderSettingsAtPlay;
        std::optional<ShadowSettings> m_ShadowSettingsAtPlay;
        std::optional<RendererSettings> m_RendererSettingsAtPlay;
        Ref<Scene> m_EditScene;
        std::optional<EditorCamera> m_Camera;
        bool m_Running = false;
        bool m_RecordOps = true;
        std::vector<UUID> m_Spawned;
        std::map<std::string, u32> m_Executed;
        FrameTimeTail m_CycleTimes{ 4096u };
        FrameTimeTail m_AllTimes{ 1u << 16u };
        Report m_Report;
        u64 m_ValidationAtStart = 0;
        fs::path m_GameRoot;

      private:
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssetManager;
        Ref<EditorAssetManager> m_EditorAssetManager;
        Ref<RuntimeAssetManager> m_RuntimeAssetManager;
        std::optional<ScopedFileRestore> m_Registry;
        fs::path m_PreviousCwd;
        ShadowSettings m_PreviousShadow{};
        bool m_SavedFault = false;
        bool m_SetUpComplete = false;
    };

    TEST_F(LifecycleSoak, EditorSequenceHoldsAcrossCycles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunSoak(Host::Editor);
    }

    TEST_F(LifecycleSoak, CookedRuntimeSequenceHoldsAcrossCycles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunSoak(Host::Runtime);
    }

    // The negative control (acceptance 3). With the activation reset skipped,
    // returning to A from B leaves B's Deferred path and the tier the cycle
    // last set, and both comparisons the soak relies on must see it: the
    // metadata outright, the image beyond A's noise floor. If either stays
    // green here, the soak's return-to-A check is not a check.
    TEST_F(LifecycleSoak, MissingActivationResetIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        m_Host = Host::Editor;
        ASSERT_NO_FATAL_FAILURE(EnterEditor());
        ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
        const Settled control = SettleAndCapture();
        const Metadata controlMetadata = ReadMetadata();

        // Healthy first, so a failure below is the fault's and not the scene's.
        ASSERT_NO_FATAL_FAILURE(Open(kSceneB));
        Frames(kStepFrames);
        ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
        const Settled healthy = SettleAndCapture();
        const FrameVerdict healthyVerdict = JudgeAgainstControl(CompareFrames(control.Image, healthy.Image), control.Floor);
        ASSERT_TRUE(healthyVerdict.Held) << "without the fault A already fails: " << healthyVerdict.Describe();
        ASSERT_EQ(ReadMetadata(), controlMetadata);

        // B is activated healthily, then the return to A loses its reset.
        ASSERT_NO_FATAL_FAILURE(Open(kSceneB));
        Frames(kStepFrames);
        ASSERT_EQ(Renderer3D::GetRendererSettings().Path, RenderingPath::Deferred) << "B is authored on Deferred";
        Levers::SetFaultSkipSceneRenderSettingsPublish(true);
        ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
        const Settled faulted = SettleAndCapture();
        Levers::SetFaultSkipSceneRenderSettingsPublish(false);

        const Metadata faultedMetadata = ReadMetadata();
        WriteEvidence("LifecycleSoakFault", "ForwardPlus_AControl", control.Image);
        WriteEvidence("LifecycleSoakFaultOn", "Deferred_AWithoutReset", faulted.Image);
        EXPECT_NE(faultedMetadata, controlMetadata)
            << "the metadata comparison did not see the missing reset: " << faultedMetadata.Describe();
        const FrameVerdict faultedVerdict = JudgeAgainstControl(CompareFrames(control.Image, faulted.Image), control.Floor);
        EXPECT_FALSE(faultedVerdict.Held) << "the image comparison did not see the missing reset: "
                                          << faultedVerdict.Describe();
        std::cout << "[LifecycleSoak] negative control: healthy " << healthyVerdict.Describe() << "; faulted "
                  << faultedVerdict.Describe() << "; metadata " << faultedMetadata.Describe() << " vs "
                  << controlMetadata.Describe() << '\n';

        // Restored: the reset is back and A is A again.
        ASSERT_NO_FATAL_FAILURE(Open(kSceneA));
        const Settled restored = SettleAndCapture();
        EXPECT_TRUE(JudgeAgainstControl(CompareFrames(control.Image, restored.Image), control.Floor).Held);
        EXPECT_EQ(ReadMetadata(), controlMetadata);
    }
} // namespace OloEngine::Tests::Lifecycle
