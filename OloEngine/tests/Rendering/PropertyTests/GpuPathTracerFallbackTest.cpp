// OLO_TEST_LAYER: integration
// =============================================================================
// GpuPathTracerFallbackTest.cpp — the GPU reference path tracer on a machine
// that CANNOT ray trace, which is the path every CI runner takes (issue
// #1055). Drives the real render pipeline on the OpenGL fixture with the
// tracer switched ON and demonstrates, rather than asserts, that:
//
//   * the frame is BYTE-IDENTICAL to the tracer-off frame — the fallback is
//     structural: the pass's target is never declared, the alias chain never
//     sees it, and nothing downstream changes;
//   * the pass exists, is not ready, reports ShaderUnavailable as its reason
//     (resolved every frame from the wiring, because a culled pass never runs Execute),
//     and the graph holds no PathTracerColor resource;
//   * the emissive gather stays inert — no table, no address — because the
//     device cannot trace.
//
// "Skips cleanly with a diagnosable reason" is the acceptance criterion, and
// the reason is what the panel shows and the log prints. Skips itself only
// for the usual reason: no GL 4.6 context at all.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PathTracing/GpuPathTracerTypes.h"
#include "OloEngine/Renderer/Passes/GpuPathTracerPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstring>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 180;
        const glm::vec3 kCameraPosition{ 0.0f, 3.0f, 8.0f };
        constexpr f32 kCameraYaw = 0.0f;
        constexpr f32 kCameraPitch = 0.3f;
    } // namespace

    class GpuPathTracerFallbackTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            auto addPrimitive = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                         const glm::vec3& scale, const glm::vec3& albedo, const glm::vec3& emissive)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            };

            addPrimitive("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 20.0f, 1.0f, 20.0f },
                         { 0.8f, 0.8f, 0.8f }, { 0.0f, 0.0f, 0.0f });
            addPrimitive("Cube", MeshPrimitive::Cube, { 0.0f, 1.0f, 0.0f }, { 2.0f, 2.0f, 2.0f },
                         { 0.2f, 0.45f, 0.8f }, { 0.0f, 0.0f, 0.0f });
            // An emitter, so the emissive gather would have something to
            // collect if it were running — which on this device it must not.
            addPrimitive("Lamp", MeshPrimitive::Cube, { 3.0f, 3.0f, 0.0f }, { 0.5f, 0.5f, 0.5f },
                         { 0.0f, 0.0f, 0.0f }, { 8.0f, 7.0f, 6.0f });
        }

        void Capture(std::vector<u8>& outPixels)
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kCameraPosition, kCameraYaw, kCameraPitch);
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
        }
    };

    TEST_F(GpuPathTracerFallbackTest, SwitchedOnWithoutARayTracingDeviceChangesNothingAndSaysWhy)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // This test is about the OpenGL fixture's answer. A Vulkan build of the
        // test binary still runs the fixture on GL, so the predicate is the
        // renderer's, not the build's.
        if (RenderCommand::SupportsRayTracing())
            GTEST_SKIP() << "This fixture reports hardware ray tracing; the fallback path is not reachable here.";

        auto& settings = Renderer3D::GetPostProcessSettings();
        settings.GpuPathTracer.Enabled = false;
        std::vector<u8> off;
        ASSERT_NO_FATAL_FAILURE(Capture(off));

        settings.GpuPathTracer.Enabled = true;
        std::vector<u8> on;
        ASSERT_NO_FATAL_FAILURE(Capture(on));

        ASSERT_EQ(on.size(), off.size());
        EXPECT_EQ(std::memcmp(on.data(), off.data(), on.size()), 0)
            << "with no ray-tracing device the tracer must leave the rasterised frame byte-identical";

        GpuPathTracerPass* pass = Renderer3D::GetGpuPathTracerPass();
        ASSERT_NE(pass, nullptr) << "the pass exists on every backend; only its shader is conditional";
        EXPECT_FALSE(pass->IsReadyForExecution());
        const GpuPathTracerStats& stats = pass->GetStats();
        EXPECT_FALSE(stats.Active);
        EXPECT_EQ(stats.Fallback, GpuPathTracerFallbackReason::ShaderUnavailable)
            << "the reason is resolved every frame from the per-frame wiring, not from Execute, which a culled pass "
               "never runs; got: "
            << ToString(stats.Fallback);
        EXPECT_FALSE(ToString(stats.Fallback).empty());

        // The graph declared no target: the structural half of the fallback.
        EXPECT_EQ(Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PathTracerColor), nullptr);

        // And the light table was never gathered — it costs nothing here.
        EXPECT_EQ(stats.EmissiveTriangles, 0u);
    }
} // namespace OloEngine::Tests
