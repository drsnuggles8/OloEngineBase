// =============================================================================
// McpPerfSettleTest.cpp
//
// Pins the #519 "Smaller findings" fix: the first perf-lever write right after
// a heavy scene load (or any render-throttled beat) didn't take effect on the
// GPU — a caller that flipped a renderer setting then immediately read
// olo_perf_snapshot saw stale pre-change data, because RendererProfiler's
// BeginFrame/EndFrame (and so GetLastCompletedFrameData(), what the snapshot
// reads) only run inside Renderer3D::BeginScene, which the editor's render-
// budget throttle skips entirely for a beat after a stall. Handle_
// RendererSettingsSet (McpTools.cpp) now waits out that transient — the same
// AwaitRenderedFrames/IsCaptureUnready settle discipline the camera-pose
// screenshot tools already use — before returning, so a caller never
// round-trips on the "changed but not yet rendered" window.
//
// This test proves the settle-wait actually executes (not just that the tool
// still returns successfully): olo_renderer_settings_set must pump at least
// `kSettingsSettleFrames` (2) more rendered frames before its response comes
// back, which is only true if AwaitRenderedFrames blocks the response as
// intended. Without the fix the call returns near-instantly (0-1 pumps).
//
// Classification: integration (real Renderer3D bring-up + in-process MCP
// dispatch via McpHeadlessHost). Sibling of McpHeadlessAttachTest.
// =============================================================================

// OLO_TEST_LAYER: integration

#include "OloEnginePCH.h"

#include "McpHeadlessHost.h"
#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "MCP/McpServer.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    namespace
    {
        using Json = OloEngine::MCP::Json;

        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 180;
    } // namespace

    // A minimal lit scene, same shape as McpHeadlessAttachTest's fixture — only
    // needs SOMETHING rendering so Renderer3D is fully initialized and
    // olo_renderer_settings_set has a live graph to act on.
    class McpPerfSettleTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity light = scene.CreateEntity("Sun");
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
            dl.m_Intensity = 3.0f;

            Entity cube = scene.CreateEntity("Cube");
            auto& mc = cube.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            cube.AddComponent<MaterialComponent>();
        }

        [[nodiscard]] EditorCamera MakeCamera() const
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 1.2f, 4.5f), /*yaw=*/0.0f, /*pitch=*/0.18f);
            return camera;
        }
    };

    TEST_F(McpPerfSettleTest, RendererSettingsSetSettlesBeforeReturning)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera = MakeCamera();
        RunEditorFrames(camera, 2); // warm the graph up before hosting

        McpHeadlessHost host(McpHeadlessHost::Hooks{
            .GetScene = [this]() -> Ref<Scene>
            { return GetSceneRef(); },
            .GetCompositeFramebuffer = []() -> Ref<Framebuffer>
            { return Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); },
            .Camera = &camera,
            .RenderWidth = kWidth,
            .RenderHeight = kHeight,
            .RenderOneFrame = [this, &camera]()
            { RunEditorFrames(camera, 1); },
        });

        const u16 port = host.Start();
        ASSERT_NE(port, 0) << "McpHeadlessHost failed to bind a port for the MCP server";

        // olo_renderer_settings_set is a consented WRITE tool.
        host.Server().SetAllowWrites(true);

        // Get past the host's own frame==0 "unready" special-case before timing
        // the settle window, so the measurement below isolates the NEW settle
        // logic in Handle_RendererSettingsSet rather than that startup case.
        host.PumpOnce();
        host.PumpOnce();

        const u64 before = host.FrameIndex();
        const Json resp = host.CallTool("olo_renderer_settings_set", Json{ { "setting", "depthprepass" }, { "value", "on" } });
        const u64 after = host.FrameIndex();

        ASSERT_TRUE(resp.contains("result")) << "olo_renderer_settings_set returned an error: " << resp.dump(2);
        EXPECT_FALSE(resp["result"].value("isError", false)) << resp.dump(2);

        // The core regression guard: the call must not return until at least a
        // couple more frames have actually rendered with the new setting
        // applied — proving AwaitRenderedFrames genuinely blocked the response
        // rather than the tool returning as soon as the write landed.
        EXPECT_GE(after - before, 2u)
            << "olo_renderer_settings_set returned without settling rendered frames (#519 regression: "
               "a caller reading olo_perf_snapshot immediately afterward could see stale pre-change data)";

        host.Stop();
    }
} // namespace OloEngine::Tests
