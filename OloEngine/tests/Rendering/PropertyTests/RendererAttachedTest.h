#pragma once

// =============================================================================
// RendererAttachedTest — fixture for Functional tests that need to drive a
// real `Scene::OnUpdateRuntime` with the full Renderer3D pipeline attached.
//
// Complements `Functional::FunctionalTest`, which runs headless (no GL
// context, no Renderer::Init). When a test asks "does the editor render
// this scene's components correctly?", headless ticks aren't enough —
// rendering side-effects (mesh visible, light contributes, material
// override applied, etc.) only manifest when Renderer3D is actually
// executing draws.
//
// Lifecycle
// ---------
//   * `SetUpTestSuite` brings up `Renderer::Init(Renderer3D, nullptr)`
//     once per process. The 42-shader pipeline compile is ~25s on a
//     dev box but happens only on the first test in the suite.
//   * Per-test `SetUp` creates a fresh `Scene` with rendering DISABLED.
//     Subclasses override `BuildScene()` to add entities and, when they
//     want the full draw path to run, call `EnableRendering(w, h)`.
//   * Per-test `TearDown` clears the Scene; the renderer stays initialised
//     for the next test.
//   * `Renderer::Shutdown()` runs once, after ALL tests in the process,
//     from the test `main()` (after RUN_ALL_TESTS) — while the shared GL
//     context is still current. (Doing it at process exit /
//     static-destruction time instead SIGSEGVs: `Renderer3D::s_Data` is a
//     static holding GL handles, and the GL context is gone by then.)
//
// Driving the real render path
// ----------------------------
//   Call `EnableRendering(w, h)` from `BuildScene()` to switch the Scene
//   into 3D mode, size the render-graph targets, and enable rendering.
//   `RunFrames` then drives the entire Renderer3D pipeline. The per-frame
//   tick is wrapped in a `GLStateGuard` (restore policy) so the full
//   pipeline leaves no global GL state behind — a render here cannot
//   corrupt the fixed-function / binding state seen by the next test in
//   the same process. After ticking, `ReadbackComposite()` returns the
//   final composited frame (the same `UIComposite` output the editor
//   viewport displays) for pixel assertions / PNG evidence.
//
// Why a separate fixture from `FunctionalTest`?
// ---------------------------------------------
//   The two axes intentionally diverge:
//     - `FunctionalTest` is *headless* (ADR 0002) — no GL context, faster,
//       parallelisable, runs on WSL. Used for cross-subsystem state
//       contracts (animation × physics × scripting × …).
//     - `RendererAttachedTest` requires a GPU, is much slower per-test,
//       and exists specifically for rendering-side-effect contracts.
//   Tests should pick the cheapest fixture that proves their contract.
//
// Skipping when no GPU available
// ------------------------------
//   Every test using this fixture must begin with `OLO_ENSURE_GPU_OR_SKIP()`.
//   On CI without a usable GL 4.6 context the test is SKIP'd (not failed).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>

#include <vector>

namespace OloEngine
{
    class EditorCamera;
}

namespace OloEngine::Tests
{
    class RendererAttachedTest : public ::testing::Test
    {
      public:
        // One-time per process: bring up Renderer3D + all 42 production
        // shaders. Idempotent across test-suite invocations because
        // `Renderer::Init` is itself idempotent (handled by ShaderWarmup /
        // Renderer3D's lazy-init guards).
        static void SetUpTestSuite();

        // Tear down the renderer when the test suite finishes. The GL
        // context from RenderPropertyFixture stays alive for the lifetime
        // of the process.
        static void TearDownTestSuite();

      protected:
        void SetUp() override;
        void TearDown() override;

        /// Construct entities. Called once per test, after the Scene is
        /// created and configured for runtime + rendering.
        virtual void BuildScene() = 0;

        /// Tick `count` frames at `dtSeconds` per frame. Each tick is a
        /// full `Scene::OnUpdateRuntime` call, which (with rendering
        /// enabled) drives the entire Renderer3D pipeline. Each tick is
        /// wrapped in a `GLStateGuard` (restore policy) so the render
        /// leaves no global GL state behind for the next test.
        void RunFrames(u32 count, f32 dtSeconds = 1.0f / 60.0f);

        /// Tick `count` frames through the EDITOR render path
        /// (`Scene::OnUpdateEditor` with the supplied posed `EditorCamera`)
        /// instead of the runtime primary camera. Mirrors `RunFrames`: each
        /// tick is wrapped in a `GLStateGuard` (restore policy) when rendering
        /// is enabled, so a full-pipeline render here leaves no global GL state
        /// behind for the next GPU test in the same process. Use this for
        /// visual tests that need an explicit fly-camera pose — multi-angle
        /// screenshot evidence, etc. — which the single runtime
        /// `CameraComponent` path cannot express.
        void RunEditorFrames(const EditorCamera& camera, u32 count, f32 dtSeconds = 1.0f / 60.0f);

        /// Opt the Scene into the full 3D draw path. Call from `BuildScene()`.
        /// Enables 3D mode, sizes the camera + the Renderer3D render-graph
        /// targets to `width`x`height`, and flips `SetRenderingEnabled(true)`.
        /// After this, `RunFrames` drives the entire pipeline and
        /// `ReadbackComposite()` can read the result back.
        void EnableRendering(u32 width = 256, u32 height = 256);

        /// Read back the final composited frame (the render graph's
        /// `UIComposite` RT0 — the same image the editor viewport shows)
        /// into a tightly-packed RGBA8 buffer. Returns false if rendering
        /// was never enabled or the composite framebuffer is unavailable.
        /// `outWidth`/`outHeight` receive the render target dimensions.
        [[nodiscard]] bool ReadbackComposite(std::vector<u8>& outRgba, u32& outWidth, u32& outHeight);

        [[nodiscard]] Scene& GetScene()
        {
            return *m_Scene;
        }
        [[nodiscard]] const Scene& GetScene() const
        {
            return *m_Scene;
        }
        [[nodiscard]] Ref<Scene> GetSceneRef() const
        {
            return m_Scene;
        }

        // Default viewport that the renderer renders into. Subclasses
        // can call `SetViewport(w, h)` from `BuildScene` before any tick.
        void SetViewport(u32 width, u32 height);

      private:
        Ref<Scene> m_Scene;
        bool m_RenderingEnabled = false;
        u32 m_RenderWidth = 0;
        u32 m_RenderHeight = 0;

        // Per-test snapshot of the process-wide renderer configuration
        // (rendering path, culling, post-process chain). Restored in
        // TearDown so a test that reconfigures the renderer — e.g. the
        // SSGI/SSR evidence tests switching to the deferred path — cannot
        // leak the change into later tests in the same process. A leaked
        // path renders every subsequent visual test through the wrong
        // pipeline (dark frames, dead assertions, altered evidence PNGs)
        // while GLStateGuard's raw-GL restore reports nothing wrong.
        RendererSettings m_SavedRendererSettings{};
        PostProcessSettings m_SavedPostProcessSettings{};
        bool m_SettingsSnapshotted = false;
    };
} // namespace OloEngine::Tests
