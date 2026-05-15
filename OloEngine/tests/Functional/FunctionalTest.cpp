#include "OloEnginePCH.h"
#include "FunctionalTest.h"

#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"

#include <cmath>
#include <fstream>
#include <stdexcept>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Functional
{

    std::filesystem::path GetEditorRoot()
    {
        const std::filesystem::path root{ OLO_TEST_EDITOR_ROOT };
        // Sanity-check at first call: a missing OloEditor/ tree means the
        // build was relocated without rebuilding, or the test binary is
        // running against a stripped checkout. Either way, file-loading
        // tests will fail mysteriously without this guard.
        static const bool s_Verified = [&]
        {
            if (!std::filesystem::exists(root))
            {
                throw std::runtime_error{
                    "OLO_TEST_EDITOR_ROOT points at a non-existent path: " + root.string()
                };
            }
            return true;
        }();
        (void)s_Verified;
        return root;
    }
    namespace
    {
        // FNV-1a-ish hash of (suite, name) into a 64-bit seed. Stable across
        // runs so a flaky test reproduces with the same RNG sequence.
        u64 HashTestName(const char* suite, const char* name)
        {
            constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL;
            constexpr u64 kFnvPrime = 0x00000100000001B3ULL;
            u64 h = kFnvOffset;
            for (const char* p = suite; p && *p; ++p)
            {
                h ^= static_cast<u8>(*p);
                h *= kFnvPrime;
            }
            h ^= '.';
            h *= kFnvPrime;
            for (const char* p = name; p && *p; ++p)
            {
                h ^= static_cast<u8>(*p);
                h *= kFnvPrime;
            }
            return h;
        }
    } // namespace

    void FunctionalTest::SetUp()
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const u64 seed = HashTestName(info ? info->test_suite_name() : "Unknown",
                                      info ? info->name() : "Unknown");
        m_Rng.SetSeed(seed);

        m_Scene = Scene::Create();
        m_ElapsedSimulated = 0.0f;
        m_Physics3DEnabled = false;
        m_Physics2DEnabled = false;
        m_LuaEnabled = false;
        m_AudioEnabled = false;
        m_AssetManagerEnabled = false;
        m_AssetProjectDir.clear();

        // Functional default is headless (ADR 0002): no GL context, so don't let
        // OnUpdateRuntime fall into the renderer branch when a Primary
        // CameraComponent exists. This mirrors what Scene::OnRuntimeStart
        // does when Application::IsHeadless() — but we never call that path
        // because it requires Application::Get(). Renderer-attached tests
        // will flip this back on in their own fixture base.
        m_Scene->SetRenderingEnabled(false);

        BuildScene();
    }

    void FunctionalTest::TearDown()
    {
        if (m_AssetManagerEnabled)
        {
            // We deliberately don't remove m_AssetProjectDir here:
            // Project::s_AssetManager is a static Ref<> that we can't null
            // out via the public API (SetAssetManager asserts non-null).
            // Its EditorAssetManager destructor will run on next-test
            // overwrite or at process exit, and tries to SerializeAssetRegistry
            // back to disk on the way out. Pulling the directory out from
            // under it spams the log with "Failed to open file" errors and
            // can interact badly with the file watcher's pending tasks. The
            // OS will reclaim <temp>/OloEngineFunctional itself.
            m_AssetManagerEnabled = false;
        }
        if (m_AudioEnabled)
        {
            // Un-install the scene's manager from the global AudioPlayback
            // singleton so subsequent tests don't see a dangling pointer
            // after this scene is destroyed.
            Audio::AudioPlayback::SetManager(nullptr);
            m_AudioEnabled = false;
        }
        if (m_LuaEnabled)
        {
            LuaScriptEngine::OnRuntimeStop();
            ScriptEngine::SetSceneContextForTesting(nullptr);
            m_LuaEnabled = false;
        }
        if (m_Physics2DEnabled && m_Scene)
        {
            m_Scene->OnPhysics2DStop();
        }
        if (m_Physics3DEnabled && m_Scene)
        {
            m_Scene->OnPhysics3DStop();
        }
        m_Scene.Reset();
        // Physics3DSystem singleton is intentionally left alive across tests
        // within a process — CreateInstance/DestroyInstance is per-process,
        // not per-test, and JoltScene Initialize/Shutdown handles per-test
        // body lifetime.
    }

    void FunctionalTest::RunFrames(u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
        {
            m_Scene->OnUpdateRuntime(ts);
            m_ElapsedSimulated += dtSeconds;
        }
    }

    void FunctionalTest::TickFor(f32 totalSeconds, f32 dtSeconds)
    {
        if (totalSeconds <= 0.0f || dtSeconds <= 0.0f)
            return;
        const u32 frames = static_cast<u32>(std::ceil(totalSeconds / dtSeconds));
        RunFrames(frames, dtSeconds);
    }

    bool FunctionalTest::TickUntil(const std::function<bool()>& predicate,
                                   f32 timeoutSeconds,
                                   f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        f32 elapsed = 0.0f;
        // Check before first tick so a predicate that's already true on entry
        // returns immediately without an off-by-one tick.
        if (predicate())
            return true;
        while (elapsed < timeoutSeconds)
        {
            m_Scene->OnUpdateRuntime(ts);
            m_ElapsedSimulated += dtSeconds;
            elapsed += dtSeconds;
            if (predicate())
                return true;
        }
        return false;
    }

    namespace
    {
        // Bring up the engine task scheduler once per process — JoltJobSystemAdapter
        // queues into it from inside Simulate(), and without started workers the
        // queued jobs never run, hanging the physics tick. Application normally
        // does this at startup; the test binary doesn't construct an Application,
        // so the harness owns the lifecycle for any test that needs Physics3D.
        void EnsureTaskSchedulerStarted()
        {
            static const bool s_Once = []
            {
                LowLevelTasks::InitGameThreadId();
                Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
                LowLevelTasks::FScheduler::Get().StartWorkers();
                // Workers stay alive for the rest of the process. There is no
                // matching StopWorkers — gtest doesn't run a destructor pass on
                // process exit anyway, and shutting them down between tests
                // would force a re-init storm.
                return true;
            }();
            (void)s_Once;
        }
    } // namespace

    void FunctionalTest::EnablePhysics2D()
    {
        m_Scene->OnPhysics2DStart();
        m_Physics2DEnabled = true;
    }

    void FunctionalTest::EnablePhysics3D()
    {
        // Physics needs the engine task scheduler running because
        // JoltJobSystemAdapter queues into it from inside Simulate().
        EnsureTaskSchedulerStarted();

        // The Physics3DSystem singleton (CreateInstance/GetInstance) is unused
        // by the rest of the engine — JoltScene owns its own JPH::PhysicsSystem
        // and InitializeJolt() bootstraps everything Jolt needs internally. So
        // the harness only needs OnPhysics3DStart, which forwards into
        // JoltScene::Initialize().
        m_Scene->OnPhysics3DStart();
        m_Physics3DEnabled = true;
    }

    void FunctionalTest::EnableAnimation()
    {
        // Animation tick lives inside Scene::OnUpdateRuntime — no init needed.
    }

    namespace
    {
        // LuaScriptEngine::Init() is process-once. We track it here so
        // multiple tests can call EnableLua() safely; the engine doesn't
        // expose an "is-initialised" probe of its own.
        bool s_LuaScriptEngineInitialised = false;
    } // namespace

    void FunctionalTest::EnableLua()
    {
        if (!s_LuaScriptEngineInitialised)
        {
            LuaScriptEngine::Init();
            s_LuaScriptEngineInitialised = true;
        }
        LuaScriptEngine::OnRuntimeStart(m_Scene.Raw());

        // The Lua bindings (LuaScriptGlue) read the scene via
        // ScriptEngine::GetSceneContext() — note: the C# ScriptEngine, not
        // LuaScriptEngine. Both engines share that scene-context pointer.
        // Production code sets it from Scene::OnRuntimeStart's C# wiring;
        // tests use the dedicated testing setter.
        ScriptEngine::SetSceneContextForTesting(m_Scene.Raw());

        // Scene::DestroyEntity gates its Lua-OnDestroy dispatch on
        // m_IsRunning. Production sets that in OnRuntimeStart; here we
        // set it directly via the test-only public setter.
        m_Scene->SetRunning(true);

        m_LuaEnabled = true;
    }

    void FunctionalTest::RegisterLuaScript(Entity entity, const std::filesystem::path& scriptPath)
    {
        ASSERT_TRUE(m_LuaEnabled) << "EnableLua() must be called before RegisterLuaScript()";

        // Ensure the entity has a LuaScriptComponent — production code reads
        // ScriptFile from this component each tick.
        if (!entity.HasComponent<LuaScriptComponent>())
        {
            entity.AddComponent<LuaScriptComponent>();
        }
        entity.GetComponent<LuaScriptComponent>().ScriptFile = scriptPath.string();

        // Register the per-entity script instance (calls OnCreate). Production
        // code calls this from Scene::OnRuntimeStart for every entity present
        // at start-of-runtime; we never call OnRuntimeStart from the harness
        // so this manual call is required.
        LuaScriptEngine::OnCreateEntity(entity, scriptPath.string());
    }

    void FunctionalTest::EnableAudio()
    {
        m_Scene->InitAudioRuntime();
        m_AudioEnabled = true;
    }

    void FunctionalTest::EnableDialogue()
    {
        m_Scene->InitDialogueSystem();
    }

    void FunctionalTest::EnableAssetManager(const std::vector<std::filesystem::path>& assetsToStage)
    {
        // Build a unique temp project per-test under <temp>/OloEngineFunctional/<suite>.<name>/
        // so parallel ctest runs don't fight over the same registry path. This
        // path becomes Project::GetProjectDirectory() — EditorAssetManager's
        // SerializeAssetRegistry will write AssetRegistry.oar HERE, not in
        // the working tree.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info ? info->test_suite_name() : "Unknown") + "." + std::string(info ? info->name() : "Unknown");

        std::error_code ec;
        m_AssetProjectDir = std::filesystem::temp_directory_path() / "OloEngineFunctional" / folder;
        std::filesystem::remove_all(m_AssetProjectDir, ec);
        std::filesystem::create_directories(m_AssetProjectDir / "Assets", ec);
        if (ec)
        {
            FAIL() << "EnableAssetManager: failed to create temp project dir at "
                   << m_AssetProjectDir.string() << ": " << ec.message();
        }

        // Stage the requested assets, preserving their project-relative paths.
        // Source root is OloEditor/SandboxProject/ — the real one. Targets land
        // under m_AssetProjectDir/<same-relative-path>.
        const auto sandboxRoot = GetEditorRoot() / "SandboxProject";
        for (const auto& relative : assetsToStage)
        {
            const auto source = sandboxRoot / relative;
            const auto dest = m_AssetProjectDir / relative;
            std::filesystem::create_directories(dest.parent_path(), ec);
            std::filesystem::copy_file(source, dest,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "EnableAssetManager: failed to stage " << source.string()
                             << " -> " << dest.string() << ": " << ec.message();
        }

        // Minimal .oloproj — ProjectSerializer reads under the top-level
        // `Project:` key, populating ProjectConfig from these scalars.
        {
            std::ofstream proj(m_AssetProjectDir / "FunctionalTest.oloproj");
            proj << "Project:\n"
                    "  Name: FunctionalTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        // Hand the project to the engine. Project::Load reads the file,
        // populates ProjectConfig, and sets m_ProjectDirectory = parent path.
        ASSERT_TRUE(Project::Load(m_AssetProjectDir / "FunctionalTest.oloproj"))
            << "Project::Load failed for temp project at " << m_AssetProjectDir.string();

        // Bring up the asset manager. Initialize() scans the asset directory,
        // populates the registry from files on disk, and (since the registry
        // started empty) calls SerializeAssetRegistry — writing
        // <tempdir>/AssetRegistry.oar. That side-effect is contained in the
        // throwaway directory which TearDown removes.
        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize();
        Project::SetAssetManager(assetManager);

        m_AssetManagerEnabled = true;
    }

    std::filesystem::path FunctionalTest::StagedAssetAbsolutePath(const std::filesystem::path& projectRelative) const
    {
        return m_AssetProjectDir / projectRelative;
    }

} // namespace OloEngine::Functional
