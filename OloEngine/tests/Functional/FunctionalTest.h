#pragma once

// =============================================================================
// FunctionalTest — base fixture for Functional / Cross-subsystem tests.
//
// Functional tests catch bugs at the seams between subsystems (Animation × Physics ×
// Scripting × Networking) by driving a real Scene through multiple Ticks of
// `Scene::OnUpdateRuntime`. Per ADR 0001 / 0002 / 0003:
//   - Default tick is headless (no GL context, no RenderScene call).
//   - Determinism is best-effort: fixed Timestep + per-test seeded RNG.
//   - Subsystems are opt-in via Enable* helpers called from BuildScene().
//
// Authoritative reference: docs/testing.md.
// Glossary: CONTEXT.md → "Functional Test", "Tick", "Latent Assertion".
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Scene/Scene.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace OloEngine::Functional
{

    /// Absolute path to the editor asset root (`OloEditor/`), independent of
    /// the test binary's current working directory. Resolved from the
    /// `OLO_TEST_EDITOR_ROOT` compile-define injected by
    /// `OloEngine/tests/CMakeLists.txt`. See ADR 0003.
    [[nodiscard]] std::filesystem::path GetEditorRoot();

    class FunctionalTest : public ::testing::Test
    {
      protected:
        void SetUp() override;
        void TearDown() override;

        /// Construct entities and call Enable* helpers. Runs once per test
        /// after the Scene is created and before the test body executes.
        virtual void BuildScene() = 0;

        // ------------------- Latent / multi-frame helpers --------------------

        /// Tick `count` simulation frames at fixed `dtSeconds`.
        void RunFrames(u32 count, f32 dtSeconds = 1.0f / 60.0f);

        /// Tick until `totalSeconds` of simulated time have elapsed (rounded
        /// up to the next whole frame).
        void TickFor(f32 totalSeconds, f32 dtSeconds = 1.0f / 60.0f);

        /// Tick until `predicate()` returns true. Times out after
        /// `timeoutSeconds` of simulated time. Returns true if the predicate
        /// was satisfied, false on timeout.
        [[nodiscard]] bool TickUntil(const std::function<bool()>& predicate,
                                     f32 timeoutSeconds,
                                     f32 dtSeconds = 1.0f / 60.0f);

        // ------------------ Subsystem opt-in (call in BuildScene) ------------

        /// Bring up Physics3DSystem (process-scoped singleton, lazy) and
        /// start the Scene's JoltScene. Bodies are created from existing
        /// Rigidbody3DComponent + collider entities at this point, so
        /// construct those *before* calling.
        void EnablePhysics3D();

        /// Start Box2D simulation on the Scene. Iterates existing
        /// Rigidbody2DComponent + collider entities and creates Box2D bodies.
        /// Independent of Physics3D — they coexist via separate worlds.
        void EnablePhysics2D();

        /// Bring up `LuaScriptEngine` (process-once Init) and bind it to
        /// the harness's Scene as the runtime context. After this returns,
        /// tests can `RegisterLuaScript(entity, scriptPath)` to attach a
        /// .lua file to an entity; subsequent `OnUpdateRuntime` ticks will
        /// invoke its `OnUpdate(entityID, ts)`.
        void EnableLua();

        /// Bind a Lua script file to an entity. The entity must already
        /// have a (possibly empty) `LuaScriptComponent`; this populates
        /// `ScriptFile` and asks the engine to create the per-entity
        /// instance. Production code does this from `Scene::OnRuntimeStart`,
        /// which the Functional-test harness deliberately doesn't call.
        void RegisterLuaScript(Entity entity, const std::filesystem::path& scriptPath);

        /// Bring up the Scene's audio runtime (AudioCommandRegistry +
        /// AudioEventsManager, position resolver, AudioListener / Source
        /// Refs for existing entities). Call AFTER constructing any
        /// AudioListenerComponent / AudioSourceComponent entities — the
        /// init iterates them once to allocate their runtime Ref<>s.
        void EnableAudio();

        /// Marker — Animation tick is integrated into Scene::OnUpdateRuntime.
        /// Present so tests can document their intended subsystem coverage.
        void EnableAnimation();

        /// Bring up `Scene::DialogueSystem` so per-tick `Update(ts)` runs
        /// (text-reveal progression, auto-trigger checks). Without this the
        /// dialogue update inside `OnUpdateRuntime` is a no-op because
        /// `m_DialogueSystem` is null.
        void EnableDialogue();

        /// Mount an isolated, throwaway editor project under the OS temp
        /// directory, copy `assetsToStage` from the real SandboxProject into
        /// its `Assets/` subdirectory, then bring up an EditorAssetManager
        /// bound to that project. Loading the real SandboxProject directly
        /// would mutate its on-disk `AssetRegistry.oar` as a side-effect of
        /// `EditorAssetManager::Initialize → SerializeAssetRegistry`. A
        /// throwaway copy keeps the working tree clean across runs.
        ///
        /// `assetsToStage` paths are relative to `OloEditor/SandboxProject/`
        /// — e.g. `"Assets/Textures/Checkerboard.png"`. Each is copied
        /// preserving its relative path inside the temp project.
        ///
        /// After this returns, `Project::GetActive()` and
        /// `Project::GetAssetManager()` are non-null, and the temp project
        /// directory is recorded for cleanup in `TearDown`. See ADR 0003.
        void EnableAssetManager(const std::vector<std::filesystem::path>& assetsToStage);

        /// Project-relative path inside the temp project (only valid after
        /// `EnableAssetManager(...)`). Useful for tests that want to call
        /// `ImportAsset` themselves on staged content.
        [[nodiscard]] std::filesystem::path StagedAssetAbsolutePath(const std::filesystem::path& projectRelative) const;

        // ------------------------- Accessors ---------------------------------

        [[nodiscard]] Scene& GetScene() { return *m_Scene; }
        [[nodiscard]] const Scene& GetScene() const { return *m_Scene; }
        /// Non-owning Ref to the harness's Scene, for APIs that need the
        /// Ref<> form (e.g. `SceneSerializer(const Ref<Scene>&)`).
        [[nodiscard]] Ref<Scene> GetSceneRef() const { return m_Scene; }
        [[nodiscard]] f32 ElapsedSimulatedSeconds() const noexcept { return m_ElapsedSimulated; }
        [[nodiscard]] FastRandomPCG& Rng() noexcept { return m_Rng; }

      private:
        Ref<Scene> m_Scene;
        FastRandomPCG m_Rng;
        f32 m_ElapsedSimulated = 0.0f;
        bool m_Physics3DEnabled = false;
        bool m_Physics2DEnabled = false;
        bool m_LuaEnabled = false;
        bool m_AudioEnabled = false;
        bool m_AssetManagerEnabled = false;
        std::filesystem::path m_AssetProjectDir;
    };

} // namespace OloEngine::Functional
