// OLO_TEST_LAYER: L8
// =============================================================================
// EditorOverlayInSimulateTest.cpp
//
// The editor's Simulate mode draws the same overlay over a 3D frame as Edit
// mode does: tilemaps, world text and the UI.
//
// Both modes render through an editor camera, and what is laid over that frame
// is drawn by the UI composite pass from a callback the scene queues ahead of
// RenderScene3D. Scene::OnUpdateEditor queued it; Scene::OnUpdateSimulation's
// 3D branch did not, so a scene's UI vanished from the viewport the moment
// Simulate started and UICompositePass warned "No render callback set" (found
// during the live verification of #1352).
//
// A screen-space UI panel in a colour nothing else in the frame has is counted
// in an Edit frame and in a Simulate frame. The Edit count is the control: it
// proves the panel is on screen and that the measure sees it.
//
// Classification: L8 (full pipeline + RGBA8 readback of the composited frame).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 256;
        constexpr f32 kDt = 1.0f / 60.0f;

        // Pixels of the panel's magenta. Nothing the editor path draws on its
        // own (sky, reference grid) is near it.
        [[nodiscard]] u32 PanelPixels(const std::vector<u8>& rgba)
        {
            u32 count = 0;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                if (rgba[i] > 200u && rgba[i + 1] < 60u && rgba[i + 2] > 200u)
                {
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    class EditorOverlayInSimulateTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity canvas = scene.CreateEntity("Canvas");
            auto& canvasComponent = canvas.AddComponent<UICanvasComponent>();
            canvasComponent.m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
            auto& canvasRect = canvas.AddComponent<UIRectTransformComponent>();
            canvasRect.m_AnchorMin = { 0.0f, 0.0f };
            canvasRect.m_AnchorMax = { 1.0f, 1.0f };
            canvasRect.m_SizeDelta = { 0.0f, 0.0f };

            // The middle half of the viewport each way: a quarter of the frame.
            Entity panel = scene.CreateEntity("Panel");
            auto& panelRect = panel.AddComponent<UIRectTransformComponent>();
            panelRect.m_AnchorMin = { 0.25f, 0.25f };
            panelRect.m_AnchorMax = { 0.75f, 0.75f };
            panelRect.m_SizeDelta = { 0.0f, 0.0f }; // stretched to its anchors, no margin
            panel.AddComponent<UIPanelComponent>().m_BackgroundColor = { 1.0f, 0.0f, 1.0f, 1.0f };
            panel.AddComponent<RelationshipComponent>().m_ParentHandle = canvas.GetUUID();
            if (!canvas.HasComponent<RelationshipComponent>())
            {
                canvas.AddComponent<RelationshipComponent>();
            }
            canvas.GetComponent<RelationshipComponent>().m_Children.Add(panel.GetUUID());
        }

        [[nodiscard]] static EditorCamera MakeCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.0f, 8.0f), 0.0f, 0.0f);
            return camera;
        }

        [[nodiscard]] u32 PanelPixelsOnScreen()
        {
            std::vector<u8> rgba;
            u32 width = 0;
            u32 height = 0;
            if (!ReadbackComposite(rgba, width, height))
            {
                ADD_FAILURE() << "the composited frame could not be read back";
                return 0u;
            }
            return PanelPixels(rgba);
        }
    };

    TEST_F(EditorOverlayInSimulateTest, SimulateDrawsTheUIThatEditDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const EditorCamera camera = MakeCamera();

        RunEditorFrames(camera, 3);
        const u32 inEdit = PanelPixelsOnScreen();

        for (u32 i = 0; i < 3u; ++i)
        {
            RunInFrame([this, &camera]()
                       { GetScene().OnUpdateSimulation(Timestep(kDt), camera); });
        }
        const u32 inSimulate = PanelPixelsOnScreen();

        std::printf("[editor-overlay] UI panel pixels: Edit %u, Simulate %u (of %u)\n", inEdit, inSimulate, kWidth * kHeight);
        // A quarter of the frame is 16,384 px; half of that leaves room for an
        // edge row either way and still cannot be met by an empty overlay.
        ASSERT_GT(inEdit, kWidth * kHeight / 8u) << "the panel is not on screen in Edit mode: this measures nothing";
        EXPECT_EQ(inSimulate, inEdit) << "Simulate did not draw the UI overlay Edit draws";
    }
} // namespace OloEngine::Tests
