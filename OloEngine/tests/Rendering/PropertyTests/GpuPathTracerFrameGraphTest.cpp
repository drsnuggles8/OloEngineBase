// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "VisualEvidenceGuards.h"
#include "../PathTracing/ReferenceSceneFixtures.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/GpuPathTracerPass.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>
#include <utility>
#include <stb_image/stb_image_write.h>

namespace OloEngine::Tests
{
    class GpuPathTracerFrameGraphTest : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }
        static constexpr u32 kWidth = 320;
        static constexpr u32 kHeight = 240;

        void SaveComposite(std::string_view pose, u32 expectedWidth = kWidth, u32 expectedHeight = kHeight)
        {
            u32 width = 0, height = 0;
            std::vector<u8> rgba;
            ASSERT_TRUE(ReadbackComposite(rgba, width, height));
            ASSERT_EQ(width, expectedWidth);
            ASSERT_EQ(height, expectedHeight);
            VisualEvidence::FlipRgbaRowsInPlace(rgba, width, height);
            std::filesystem::create_directories("assets/tests/visual");
            const auto file = std::format("assets/tests/visual/{}{}_{}.png", TestRenderBackend::EvidenceName("GpuPathTracerFull"),
                                          ToToken(GetParam()), pose);
            ASSERT_NE(stbi_write_png(file.c_str(), width, height, 4, rgba.data(), width * 4), 0);
        }

        void CaptureFrameCosts(const EditorCamera& camera, const RenderGraph& graph)
        {
            // Emit rows in the gtest XML. GPU observations have their own resolved-frame
            // identity and age; a missing timestamp cannot be counted as zero cost.
            auto frames = nlohmann::json::array();
            for (u32 frame = 0; frame < 12; ++frame)
            {
                RunEditorFrames(camera, 1);
                const auto memory = graph.MeasureScreenMemory();
                const auto gpu = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
                nlohmann::json row{
                    { "observation", frame }, { "cpuFrame", gpu.CurrentFrameNumber }, { "gpuFrame", gpu.FrameNumber }, { "gpuAge", gpu.AgeFrames }, { "transientDemandBytes", memory.TransientDemandBytes }, { "targetBytes", memory.NodeTargetBytes }
                };
                if (gpu.Frame.IsValid())
                    row["gpuMs"] = gpu.Frame.GpuMs;
                for (const auto& timing : graph.GetLastExecutionTimings())
                    row["passCpuMs"][timing.NodeName.ToStdString()] = timing.CpuMs;
                for (const auto& timing : gpu.Passes)
                    if (!timing.IsSubPass && timing.Sample.IsValid())
                        row["passGpuMs"][timing.Name.ToStdString()] = timing.Sample.GpuMs;
                frames.push_back(std::move(row));
            }
            RecordProperty("frameCosts", frames.dump());
        }

        void CheckWithheldFrame(const EditorCamera& camera, RenderGraph& graph, GpuPathTracerPass& tracer)
        {
            // Vegetation can withhold a previously published TLAS during the build,
            // after the pass verdict used to compile the graph. Inject that producer
            // verdict at its publication point without an entire vegetation workload.
            using HistoryPlanes = std::array<std::vector<f32>, 4>;
            const auto readHistories = [&](HistoryPlanes& planes)
            {
                const auto target = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PathTracerColor);
                ASSERT_TRUE(target);
                for (u32 plane = 0; plane < planes.size(); ++plane)
                    ReadbackRgbaFloat(target->GetColorAttachmentHandle(plane + 1), kWidth, kHeight, planes[plane]);
            };
            HistoryPlanes previous;
            ASSERT_NO_FATAL_FAILURE(readHistories(previous));
            struct ScopedSnapshot
            {
                RenderGraphPassSnapshot Snapshot;
                ~ScopedSnapshot()
                {
                    Snapshot.Disarm();
                    Snapshot.ReleaseScratch();
                }
            } capture;
            TDoubleLinkedList<RenderGraphPassSnapshot::Request> requests;
            requests.AddTail(RenderGraphPassSnapshot::Request("RasterInput", [&]()
                                                              { return graph.ResolveTextureHandle(tracer.GetPrimaryInputTextureHandle()); }));
            requests.AddTail(RenderGraphPassSnapshot::Request("TracerOutput", [&]()
                                                              { return graph.ResolveTextureHandle(tracer.GetPrimaryOutputTextureHandle()); }));
            capture.Snapshot.Arm(&graph, "GpuPathTracerPass", std::move(requests));
            struct RestoreTraceScene
            {
                RenderGraph& Graph;
                ~RestoreTraceScene()
                {
                    Graph.RemovePostPassHook("WithholdTraceScene");
                    Renderer3D::GetRayTracingScene().SetVegetationReady(true);
                }
            };
            {
                const RestoreTraceScene restore{ graph };
                graph.AddPostPassHook("WithholdTraceScene", [](std::string_view pass, RenderGraph&)
                                      {
                if (pass == "RayTracingScenePass")
                    Renderer3D::GetRayTracingScene().SetVegetationReady(false); });
                RunEditorFrames(camera, 1);
            }
            EXPECT_EQ(tracer.GetStats().Fallback, GpuPathTracerFallbackReason::AccelerationStructureWithheld);
            EXPECT_EQ(tracer.GetStats().SamplesTracedThisFrame, 0u);
            HistoryPlanes retained;
            ASSERT_NO_FATAL_FAILURE(readHistories(retained));
            for (sizet plane = 0; plane < retained.size(); ++plane)
            {
                ASSERT_EQ(previous[plane].size(), retained[plane].size());
                EXPECT_EQ(std::memcmp(previous[plane].data(), retained[plane].data(), retained[plane].size() * sizeof(f32)), 0) << plane;
            }

            // Compare both ends of the pass on the SAME frame. Comparing final composites
            // on successive frames also compares all of the downstream temporal state.
            const auto& captures = capture.Snapshot.GetResults();
            ASSERT_EQ(captures.Num(), 2);
            ASSERT_TRUE(captures[0].Captured) << captures[0].Error.ToStdString();
            ASSERT_TRUE(captures[1].Captured) << captures[1].Error.ToStdString();
            std::vector<f32> raster, fallback;
            ReadbackRgbaFloat(captures[0].Handle, kWidth, kHeight, raster);
            ReadbackRgbaFloat(captures[1].Handle, kWidth, kHeight, fallback);
            ASSERT_EQ(raster.size(), fallback.size());
            u32 differentPixels = 0u;
            f32 maxDifference = 0.0f;
            for (sizet pixel = 0; pixel < raster.size(); pixel += 4)
            {
                if (std::memcmp(raster.data() + pixel, fallback.data() + pixel, 3 * sizeof(f32)) != 0)
                    ++differentPixels;
                for (sizet channel = 0; channel < 3; ++channel)
                    maxDifference = std::max(maxDifference, std::abs(raster[pixel + channel] - fallback[pixel + channel]));
            }
            EXPECT_EQ(differentPixels, 0u) << "maximum RGB difference " << maxDifference;
        }

        void BuildScene() override
        {
            if (!RenderCommand::SupportsRayTracing())
                return;
            EnableRendering(kWidth, kHeight);
            // Use the exact geometry and materials of the CPU/device parity fixture,
            // through scene extraction and the production acceleration-structure pass.
            const auto fixture = PathTracingFixtures::MakeCornellBoxScene(18.0f, PBRModel::ClosureV2);
            RunInFrame([this, &fixture]
                       { AddSurfaces(fixture); });
        }
        void AddSurfaces(const PathTracingFixtures::CornellBoxScene& fixture)
        {
            for (const auto& instance : fixture.Scene.GetInstances())
            {
                const auto& geometry = fixture.Scene.GetGeometry(instance.GeometryIndex);
                const auto vertices = geometry.GetVertices();
                const auto indices = geometry.GetIndices();
                auto source = Ref<MeshSource>::Create(
                    TArray<Vertex>(vertices.data(), static_cast<i32>(vertices.size())),
                    TArray<u32>(indices.data(), static_cast<i32>(indices.size())));
                Submesh submesh;
                submesh.m_VertexCount = static_cast<u32>(vertices.size());
                submesh.m_IndexCount = static_cast<u32>(indices.size());
                submesh.m_BoundingBox = geometry.GetLocalBounds();
                source->AddSubmesh(submesh);
                source->Build();
                auto entity = GetScene().CreateEntity("Cornell surface");
                entity.GetComponent<TransformComponent>().SetTransform(instance.Transform);
                entity.AddComponent<MeshComponent>(source);
                const auto& material = fixture.Scene.GetMaterial(instance.MaterialIndex);
                auto& surface = entity.AddComponent<MaterialComponent>().m_Material;
                surface.SetBaseColorFactor(glm::vec4(material.BaseColor, 1.0f));
                surface.SetEmissiveFactor(glm::vec4(material.Emissive, 0.0f));
                surface.SetMetallicFactor(material.Metallic);
                surface.SetRoughnessFactor(material.Roughness);
                surface.SetPBRModel(material.Model);
            }
        }
    };

    TEST_P(GpuPathTracerFrameGraphTest, CornellSurfacesAccumulateThroughTheProductionGraph)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!RenderCommand::SupportsRayTracing())
            GTEST_SKIP() << "Reference transport needs hardware ray queries.";
        Renderer3D::GetRendererSettings().Path = GetParam();
        RunInFrame([]
                   { Renderer3D::ApplyRendererSettings(); });
        auto& settings = Renderer3D::GetPostProcessSettings().GpuPathTracer;
        settings.Enabled = true;
        settings.SamplesPerFrame = 1;
        settings.Seed = 974;
        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / kHeight, 0.1f, 100.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose(PathTracingFixtures::CornellBoxScene::EyePosition(), 0.0f, 0.0f);
        auto* tracer = Renderer3D::GetGpuPathTracerPass();
        ASSERT_NE(tracer, nullptr);
        // Scene reset can withhold the TLAS until the vegetation recovery window closes.
        // Count actual accumulated samples before taking any timing or image evidence.
        for (u32 frame = 0; frame < 32; ++frame)
        {
            RunEditorFrames(camera, 1);
            if (tracer->GetStats().Active && tracer->GetStats().AccumulatedSamplesPerPixel > 1u)
                break;
        }
        ASSERT_TRUE(tracer->GetStats().Active) << ToString(tracer->GetStats().Fallback);
        EXPECT_GT(tracer->GetStats().AccumulatedSamplesPerPixel, 1u);
        EXPECT_GT(tracer->GetStats().EmissiveTriangles, 0u);
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        ASSERT_EQ(static_cast<bool>(graph->GetNode<RenderGraphNode>("DeferredLightingPass")), GetParam() == RenderingPath::Deferred);
        CaptureFrameCosts(camera, *graph);
        const auto output = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PathTracerColor);
        ASSERT_TRUE(output);
        std::vector<f32> sums;
        ReadbackRgbaFloat(output->GetColorAttachmentHandle(1), kWidth, kHeight, sums);
        ASSERT_EQ(sums.size(), static_cast<sizet>(kWidth) * kHeight * 4);
        // RGBA32F stores one exact integer count per pixel, including every miss.
        const auto expected = tracer->GetStats().AccumulatedSamplesPerPixel + tracer->GetStats().SamplesTracedThisFrame;
        u32 incorrectCounts = 0, litPixels = 0;
        for (sizet pixel = 0; pixel < sums.size(); pixel += 4)
        {
            if (std::abs(sums[pixel + 3] - static_cast<f32>(expected)) > 0.25f || !std::isfinite(sums[pixel + 3]))
                ++incorrectCounts;
            if (sums[pixel] > 0.0f)
                ++litPixels;
        }
        EXPECT_EQ(incorrectCounts, 0u);
        EXPECT_GT(litPixels, kWidth * kHeight / 4);
        ASSERT_NO_FATAL_FAILURE(SaveComposite("Front"));

        const glm::vec3 eye = PathTracingFixtures::CornellBoxScene::RakingEyePosition();
        const glm::vec3 direction = glm::normalize(glm::vec3(-0.35f, -0.90f, -0.45f) - eye);
        camera.SetPose(eye, std::atan2(direction.x, -direction.z), -std::asin(direction.y));
        RunEditorFrames(camera, 1);
        EXPECT_EQ(tracer->GetStats().AccumulatedSamplesPerPixel, 0u);
        EXPECT_EQ(tracer->GetStats().SamplesTracedThisFrame, 1u);
        RunEditorFrames(camera, 12);
        EXPECT_EQ(tracer->GetStats().AccumulatedSamplesPerPixel, 12u);
        ASSERT_NO_FATAL_FAILURE(SaveComposite("Raking"));

        const auto materials = GetScene().GetAllEntitiesWith<MaterialComponent>();
        ASSERT_NE(materials.begin(), materials.end());
        Entity{ *materials.begin(), &GetScene() }.GetComponent<MaterialComponent>().m_Material.SetBaseColorFactor(
            glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
        RunEditorFrames(camera, 1);
        EXPECT_EQ(tracer->GetStats().AccumulatedSamplesPerPixel, 0u);
        EXPECT_EQ(tracer->GetStats().SamplesTracedThisFrame, 1u);

        ASSERT_NO_FATAL_FAILURE(CheckWithheldFrame(camera, *graph, *tracer));

        settings.Enabled = false;
        RunEditorFrames(camera, 1);
        EXPECT_EQ(tracer->GetStats().Fallback, GpuPathTracerFallbackReason::NotRequested);
        EXPECT_EQ(Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PathTracerColor), nullptr);
        settings.Enabled = true;
        settings.DebugView = GpuPathTracerDebugView::SampleCount;
        settings.SampleCountDisplayScale = 0.125f;
        RunEditorFrames(camera, 4);
        constexpr u32 resizedWidth = 384, resizedHeight = 216;
        ResizeRenderTarget(resizedWidth, resizedHeight);
        camera.SetViewportSize(resizedWidth, resizedHeight);
        RunEditorFrames(camera, 1);
        ASSERT_TRUE(tracer->GetStats().Active);
        EXPECT_EQ(tracer->GetStats().AccumulatedSamplesPerPixel, 0u);
        EXPECT_EQ(tracer->GetStats().SamplesTracedThisFrame, 1u);
        const auto resized = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PathTracerColor);
        ASSERT_TRUE(resized);
        ASSERT_EQ(resized->GetSpecification().Width, resizedWidth);
        ASSERT_EQ(resized->GetSpecification().Height, resizedHeight);
        std::vector<f32> countView;
        ReadbackRgbaFloat(resized->GetColorAttachmentHandle(0), resizedWidth, resizedHeight, countView);
        ASSERT_EQ(countView.size(), static_cast<sizet>(resizedWidth) * resizedHeight * 4u);
        EXPECT_FLOAT_EQ(countView[0], settings.SampleCountDisplayScale);
        ASSERT_NO_FATAL_FAILURE(SaveComposite("ResizedSampleCount", resizedWidth, resizedHeight));
    }

    INSTANTIATE_TEST_SUITE_P(ReferencePaths, GpuPathTracerFrameGraphTest,
                             ::testing::ValuesIn(kRenderingPaths));
} // namespace OloEngine::Tests
