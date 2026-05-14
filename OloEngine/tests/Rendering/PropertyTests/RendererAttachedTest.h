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
//   * Per-test `SetUp` creates a fresh `Scene` with rendering enabled,
//     `m_IsRunning = true`, and a default viewport. Subclasses override
//     `BuildScene()` to add entities.
//   * Per-test `TearDown` clears the Scene; the renderer stays initialised
//     for the next test.
//   * `TearDownTestSuite` calls `Renderer::Shutdown()`.
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
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>

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
        /// enabled) drives the entire Renderer3D pipeline.
        void RunFrames(u32 count, f32 dtSeconds = 1.0f / 60.0f);

        [[nodiscard]] Scene& GetScene() { return *m_Scene; }
        [[nodiscard]] const Scene& GetScene() const { return *m_Scene; }
        [[nodiscard]] Ref<Scene> GetSceneRef() const { return m_Scene; }

        // Default viewport that the renderer renders into. Subclasses
        // can call `SetViewport(w, h)` from `BuildScene` before any tick.
        void SetViewport(u32 width, u32 height);

      private:
        Ref<Scene> m_Scene;
    };
} // namespace OloEngine::Tests
