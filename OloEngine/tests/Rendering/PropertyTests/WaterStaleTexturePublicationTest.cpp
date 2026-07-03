// OLO_TEST_LAYER: L4
// =============================================================================
// WaterStaleTexturePublicationTest.cpp — regression for issue #505.
//
// Root cause pinned by a synchronous-GL-debug stack capture: WaterRenderPass
// publishes its depth FB's raw GL texture name through
// `Renderer3D::SetWaterSurfaceDepthTextureID`, and cleared it only at the top
// of its own Execute. When the render graph CULLS the water pass (any frame
// with no water submissions), Execute never runs, so the publication from the
// last water frame survives — across scenes and, in the test binary, across
// tests. Meanwhile render-graph churn (a resize / reconfigure) deletes the
// framebuffer that owned the name. ToneMapRenderPass binds the published id
// unconditionally for its underwater-fog stage, so every subsequent
// full-pipeline frame re-binds a deleted texture name:
// `GL_INVALID_OPERATION (id 1282) "<texture> is not a valid texture name"` —
// correct pixels (the fog branch never samples above water), dirty error
// queue. `PlanarReflectionTextureID` had the identical lifecycle.
//
// The fix makes both publications strictly per-frame: cleared in
// `RenderPipeline::PrepareFrame` (BeginScene), re-published only by an
// executing pass. This test pins that contract deterministically:
//
//   1. Render a water frame → the publication must be non-zero.
//   2. Remove the water entity, render again → the publication must be 0
//      (this is the assert that fails pre-fix: the stale id survived).
//   3. Resize the render-graph targets (the churn that deletes the old water
//      depth FB) and render more frames → the fixture's strict per-tick GL
//      check fails on any stale-name bind, closing the loop on the original
//      symptom rather than just the publication value.
// =============================================================================

#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 160;
        constexpr u32 kHeight = 120;
    } // namespace

    class WaterDepthPublicationScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);

            Scene& scene = GetScene();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.5f));
                dl.m_Intensity = 2.0f;
            }

            {
                m_Water = scene.CreateEntity("Ocean");
                auto& wc = m_Water.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 50.0f;
                wc.m_WorldSizeZ = 50.0f;
                wc.m_GridResolutionX = 32;
                wc.m_GridResolutionZ = 32;
            }
        }

        [[nodiscard]] EditorCamera MakePosedCamera() const
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Above the water plane (y = 0), looking down at it so the water
            // geometry is comfortably inside the frustum.
            camera.SetPose({ 0.0f, 8.0f, 25.0f }, 0.0f, -0.3f);
            return camera;
        }

        Entity m_Water;
    };

    TEST_F(WaterDepthPublicationScene, WaterDepthPublicationDoesNotOutliveWaterFrames)
    {
        const EditorCamera camera = MakePosedCamera();

        // 1. Water frame: WaterRenderPass executes its surface-depth capture and
        //    publishes the depth texture for the tone-map underwater-fog stage.
        RunEditorFrames(camera, 2);
        EXPECT_NE(Renderer3D::GetWaterSurfaceDepthTextureID(), 0u)
            << "A frame that renders water must publish the water-surface depth "
               "texture (otherwise this test exercises nothing).";

        // 2. No-water frame: the graph culls the water pass, so nothing
        //    re-publishes. The per-frame reset in PrepareFrame must have cleared
        //    the previous frame's id — a stale non-zero value here is exactly
        //    the #505 lifetime bug.
        GetScene().DestroyEntity(m_Water);
        RunEditorFrames(camera, 1);
        EXPECT_EQ(Renderer3D::GetWaterSurfaceDepthTextureID(), 0u)
            << "The water-surface depth publication survived a frame whose graph "
               "culled the water pass — a later consumer would bind a texture "
               "name it no longer owns (issue #505).";
        EXPECT_EQ(Renderer3D::GetPlanarReflectionTextureID(), 0u)
            << "The planar-reflection publication has the same per-frame "
               "contract as the water-surface depth (issue #505).";

        // 3. The churn that made the stale id dangerous in the wild: resizing
        //    the render-graph targets recreates the water depth FB, deleting the
        //    GL texture the stale publication pointed at. With the per-frame
        //    reset in place these frames bind no stale name; the fixture's
        //    strict per-tick GL-error check fails the test if any pass still
        //    re-binds a dead texture.
        SetViewport(kWidth * 2, kHeight * 2);
        Renderer3D::OnWindowResize(kWidth * 2, kHeight * 2);
        RunEditorFrames(camera, 2);
    }
} // namespace OloEngine::Tests
