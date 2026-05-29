// =============================================================================
// RendererAttachedSmokeTest.cpp
//
// Foundation smoke tests for the `RendererAttachedTest` fixture. Current
// scope: verify that `Renderer::Init(Renderer3D, nullptr)` succeeds in a
// test process AND that `Scene::OnUpdateRuntime` can still tick after
// the renderer has been initialised. This catches:
//
//   - A future regression where Renderer3D::Init introduces a hard
//     dependency on `Application::Get()` (already once: see the
//     PostProcessRenderPass null-safety fix that landed alongside this
//     fixture).
//   - A future regression where some renderer state initialised by
//     Renderer3D::Init breaks cross-subsystem Scene ticks for everyone.
//
// Out of scope (currently): verifying actual rendered pixels. The Scene's
// `m_RenderingEnabled = true` branch in OnUpdateRuntime has additional
// engine coupling that needs to be untangled before rendering-correctness
// smoke tests can run here — see the comment in
// `RendererAttachedTest::SetUp`. Until then, rendering-correctness tests
// continue to use the lower-level `Renderer3D::Submit*` path exercised by
// the L8 GoldenImage tests.
// =============================================================================

#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // EmptyScene
    //
    // No entities. Scene::OnUpdateRuntime's rendering branch skips when no
    // main CameraComponent is present, so this exercises the pre-render
    // bookkeeping (subsystem ticks) without firing the full draw path.
    // -------------------------------------------------------------------------
    class EmptyScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        { /* deliberately empty */
        }
    };

    TEST_F(EmptyScene, RendererInitAndTickDoNotCrash)
    {
        RunFrames(3);
        SUCCEED() << "Empty scene ticked 3 frames after Renderer3D::Init.";
    }

    // -------------------------------------------------------------------------
    // SceneWithCamera
    //
    // Adds a Primary CameraComponent so Scene::OnUpdateRuntime takes the
    // rendering branch. No renderable geometry — Renderer3D issues a clear
    // and exits the pass. This is the smallest test that exercises the
    // full draw-pipeline plumbing.
    // -------------------------------------------------------------------------
    class SceneWithCamera : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("MainCamera");

            // Place the camera behind the origin looking at -Z, so anything
            // we render at origin is in view.
            auto& xf = camera.GetComponent<TransformComponent>();
            xf.Translation = { 0.0f, 0.0f, 3.0f };

            auto& cc = camera.AddComponent<CameraComponent>();
            cc.Primary = true;
        }
    };

    TEST_F(SceneWithCamera, TickDoesNotCrash)
    {
        RunFrames(3);
        SUCCEED() << "Scene with primary camera ticked 3 frames after Renderer3D::Init.";
    }

    // -------------------------------------------------------------------------
    // SceneRenders3D — issue #258
    //
    // Drives the FULL Scene::OnUpdateRuntime 3D render path inside the fixture
    // (3D mode + sized render-graph targets + primary camera, via
    // EnableRendering). This is the path the fixture historically could not
    // run: it left global GL state corrupted for later tests and the process
    // SIGSEGV'd at teardown. Both are now contained (GLStateGuard-wrapped
    // ticks + the renderer-shutdown environment).
    // -------------------------------------------------------------------------
    class SceneRenders3D : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("MainCamera");
            auto& xf = camera.GetComponent<TransformComponent>();
            xf.Translation = { 0.0f, 0.0f, 3.0f };
            auto& cc = camera.AddComponent<CameraComponent>();
            cc.Primary = true;

            EnableRendering(256, 256);
        }
    };

    TEST_F(SceneRenders3D, FullPipelineTickDoesNotCrash)
    {
        RunFrames(2);
        SUCCEED() << "Scene rendered the full 3D pipeline for 2 frames without crashing.";
    }

    // Regression contract for the GL-state hygiene that makes rendering in
    // this fixture safe for the rest of the process: after a full-pipeline
    // render, the global fixed-function + binding state the caller sees must
    // be unchanged. Per-slot texture / UBO bindings are excluded — GLStateGuard
    // deliberately does not restore those (cost), and they are benign because
    // downstream GPU tests bind their own resources before drawing.
    TEST_F(SceneRenders3D, RenderLeavesNoGlobalGlStateBehind)
    {
        const GLStateSnapshot before = GLStateSnapshot::Capture();
        RunFrames(2);
        const GLStateSnapshot after = GLStateSnapshot::Capture();

        std::vector<std::string> coreDiffs;
        for (const auto& d : before.DiffAgainst(after))
        {
            const bool isPerSlotBinding =
                d.starts_with("Texture2D[") || d.starts_with("Texture2DArray[") ||
                d.starts_with("TextureCubeMap[") || d.starts_with("UBO[");
            if (!isPerSlotBinding)
                coreDiffs.push_back(d);
        }

        if (!coreDiffs.empty())
        {
            std::string joined;
            for (const auto& d : coreDiffs)
                joined += "\n    " + d;
            ADD_FAILURE() << "Full-pipeline render leaked " << coreDiffs.size()
                          << " core GL-state field(s):" << joined;
        }
    }
} // namespace OloEngine::Tests
