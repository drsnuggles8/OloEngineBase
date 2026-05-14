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

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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
        void BuildScene() override { /* deliberately empty */ }
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
} // namespace OloEngine::Tests
