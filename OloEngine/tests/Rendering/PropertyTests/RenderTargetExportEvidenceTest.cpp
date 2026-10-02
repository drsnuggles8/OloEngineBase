// OLO_TEST_LAYER: plumbing
// =============================================================================
// RenderTargetExportEvidenceTest.cpp — issue #1332.
//
// Renders real frames through the real pipeline with the copy ledger on and
// reports, per cell, every GPU image copy and framebuffer blit the frame graph
// issued: which pass, from which attachment, into which graph resource, how
// many bytes. Beside that, the transient pool's capacity, demand and alias
// savings, the frame's peak resident bytes, and the median GPU time of every
// pass. That is the issue's before/after report: copy count, bytes, GPU time
// and peak allocation for forward, deferred, MSAA, upscale and late-geometry
// frames.
//
// Each cell writes OloEditor/assets/tests/exports/RenderTargetExports_<cell>.json,
// the artefact the PR's verification matrix cites. Skips cleanly without a GL
// 4.6 context (never DISABLED_).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RendererAttachedTest.h"
#include "RendererStateCheck.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RenderTargetCopyLedger.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <stb_image/stb_image_write.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The editor's representative size: bytes and GPU time scale with it.
        constexpr u32 kWidth = 1920;
        constexpr u32 kHeight = 1080;
        constexpr u32 kWarmFrames = 6;
        constexpr u32 kMeasureFrames = 12;

        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        const glm::vec3 kEye(128.0f, 12.0f, 150.0f);

        class ScopedLever
        {
          public:
            ScopedLever(bool (*get)(), void (*set)(bool), const bool value) : m_Set(set), m_Previous(get())
            {
                m_Set(value);
            }
            ~ScopedLever()
            {
                m_Set(m_Previous);
            }
            ScopedLever(const ScopedLever&) = delete;
            ScopedLever& operator=(const ScopedLever&) = delete;

          private:
            void (*m_Set)(bool);
            bool m_Previous;
        };

        [[nodiscard]] f64 Median(std::vector<f64> values)
        {
            if (values.empty())
                return -1.0;
            std::ranges::sort(values);
            return values[values.size() / 2];
        }
    } // namespace

    class RenderTargetExportEvidence : public RendererAttachedTest
    {
      protected:
        RendererState::Snapshot m_SavedState;
        TArray<Entity> m_LateGeometry;
        Entity m_Foliage;

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            RendererState::Restore(m_SavedState);
        }

        void BuildScene() override
        {
            ASSERT_TRUE(RendererState::Capture(m_SavedState));
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity sun = scene.CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
            light.m_Intensity = 3.0f;

            // The opaque ScenePass geometry: terrain and two cubes.
            m_Foliage = scene.CreateEntityWithUUID(UUID(1332), "Terrain");
            auto& terrain = m_Foliage.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 11;
            terrain.m_ProceduralResolution = 128;
            terrain.m_ProceduralOctaves = 4;
            terrain.m_ProceduralFrequency = 1.5f;
            terrain.m_WorldSizeX = 256.0f;
            terrain.m_WorldSizeZ = 256.0f;
            terrain.m_HeightScale = 6.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (auto layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);

            for (i32 i = 0; i < 2; ++i)
            {
                Entity cube = scene.CreateEntity("Cube" + std::to_string(i));
                auto& mc = cube.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& tc = cube.GetComponent<TransformComponent>();
                tc.Translation = { 134.0f + 4.0f * static_cast<f32>(i), 8.0f, 136.0f };
                tc.Scale = glm::vec3(2.0f);
            }
        }

        // The late geometry writers of the issue: foliage, the GPU-driven
        // instanced batches, decals and water, all drawn after ScenePass.
        // Water animates every frame, so a determinism check leaves it out;
        // foliage refreshes the copied exports, so a check of what an unrefreshed
        // writer leaves behind leaves it out.
        void SetLateGeometry(const bool present, const bool water = true, const bool foliagePresent = true)
        {
            Scene& scene = GetScene();
            for (Entity entity : m_LateGeometry)
                scene.DestroyEntity(entity);
            m_LateGeometry.Reset();
            if (m_Foliage.HasComponent<FoliageComponent>())
                m_Foliage.RemoveComponent<FoliageComponent>();
            Renderer3D::EnableHZBOcclusionCulling(present);
            if (!present)
                return;

            if (foliagePresent)
                AddFoliage();
            AddOtherLateGeometry(water);
        }

        void AddFoliage()
        {
            auto& foliage = m_Foliage.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;
            FoliageLayer pines;
            pines.Name = "Pines";
            pines.MeshPath = kPineMesh;
            pines.AlbedoPath = kFoliageAlbedo;
            pines.Density = 0.02f;
            pines.SplatmapChannel = -1;
            pines.MaxSlopeAngle = 60.0f;
            pines.MinHeight = 8.0f;
            pines.MaxHeight = 12.0f;
            pines.ViewDistance = 400.0f;
            pines.FadeStartDistance = 360.0f;
            pines.UseAuthoredMesh = true;
            pines.AlphaCutoff = 0.25f;
            pines.BaseColor = glm::vec3(0.18f, 0.42f, 0.14f);
            foliage.m_Layers.Add(pines);
            foliage.m_NeedsRebuild = true;
        }

        void AddOtherLateGeometry(const bool water)
        {
            Scene& scene = GetScene();
            // > 1024 instances: the batch routes through the GPU cull, and
            // with HZB occlusion on, through GPUDrivenOcclusionPass.
            Entity field = scene.CreateEntity("InstancedField");
            auto& instanced = field.AddComponent<InstancedMeshComponent>();
            instanced.MeshSource = MeshPrimitives::CreateCube()->GetMeshSource();
            constexpr i32 kGrid = 34;
            instanced.Instances.Reserve(static_cast<sizet>(kGrid) * kGrid);
            for (i32 gy = 0; gy < kGrid; ++gy)
            {
                for (i32 gx = 0; gx < kGrid; ++gx)
                {
                    InstanceData instance;
                    instance.Transform = glm::scale(glm::translate(glm::mat4(1.0f),
                                                                   glm::vec3(118.0f + 0.6f * static_cast<f32>(gx),
                                                                             8.0f + 0.25f * static_cast<f32>(gy), 118.0f)),
                                                    glm::vec3(0.2f));
                    instance.PrevTransform = instance.Transform;
                    instanced.Instances.Add(instance);
                }
            }
            m_LateGeometry.Add(field);

            if (water)
            {
                Entity ocean = scene.CreateEntity("Water");
                ocean.GetComponent<TransformComponent>().Translation = { 128.0f, 5.0f, 128.0f };
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 24.0f;
                wc.m_WorldSizeZ = 24.0f;
                wc.m_GridResolutionX = 32;
                wc.m_GridResolutionZ = 32;
                m_LateGeometry.Add(ocean);
            }

            Entity decal = scene.CreateEntity("Decal");
            decal.GetComponent<TransformComponent>().Translation = { 122.0f, 4.0f, 140.0f };
            auto& dc = decal.AddComponent<DecalComponent>();
            dc.m_Size = { 6.0f, 8.0f, 6.0f };
            dc.m_Color = { 0.9f, 0.1f, 0.1f, 1.0f };
            dc.m_Mode = DecalMode::Albedo;
            m_LateGeometry.Add(decal);
        }

        void Frames(const u32 count)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kEye, 0.0f, 0.06f);
            RunEditorFrames(camera, count);
        }

        static void WriteArtefact(const std::string& cell, const nlohmann::json& report)
        {
            const fs::path dir = fs::path("assets") / "tests" / "exports";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.string() << ": " << ec.message();
            const fs::path file = dir / ("RenderTargetExports_" + cell + ".json");
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open()) << "cannot open " << file.string();
            out << report.dump(2) << '\n';
            out.close();
            // A failed write would leave the committed artefact stale while the
            // test passed.
            ASSERT_FALSE(out.fail()) << "writing " << file.string() << " failed";
        }
    };

    // -------------------------------------------------------------------------
    // AC1: the report, one artefact per cell. Every copy into a graph resource
    // must have been declared by the pass that issued it: an undeclared write is
    // one no edge orders and no barrier covers.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, EveryCellReportsItsCopiesBytesTimeAndPeak)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedLever ledger(&Levers::RenderGraphCopyLedger, &Levers::SetRenderGraphCopyLedger, true);

        struct Cell
        {
            const char* Name;
            RenderingPath Path;
            u32 Msaa;
            UpscaleMode Upscale;
        };
        static constexpr std::array<Cell, 6> kCells = { {
            { "GL_Forward", RenderingPath::Forward, 1u, UpscaleMode::Off },
            { "GL_ForwardPlus", RenderingPath::ForwardPlus, 1u, UpscaleMode::Off },
            { "GL_Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off },
            { "GL_Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off },
            { "GL_Forward_UpscalePerformance", RenderingPath::Forward, 1u, UpscaleMode::Performance },
            { "GL_Deferred_UpscalePerformance", RenderingPath::Deferred, 1u, UpscaleMode::Performance },
        } };

        auto& post = Renderer3D::GetPostProcessSettings();
        // Every consumer family the issue names: AO, a temporal resolve on
        // velocity, motion blur.
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        post.TAAEnabled = true;
        post.MotionBlurEnabled = true;

        for (const Cell& cell : kCells)
        {
            for (const bool late : { false, true })
            {
                const std::string name = std::string(cell.Name) + (late ? "_LateGeometry" : "");
                SCOPED_TRACE(name);
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = cell.Path;
                settings.Deferred.MSAASampleCount = cell.Msaa;
                post.Upscale = cell.Upscale;
                Renderer3D::ApplyRendererSettings();
                if (Renderer3D::GetRendererSettings().Deferred.MSAASampleCount != cell.Msaa)
                {
                    std::cout << "[ exports ] " << name << ": device returned "
                              << Renderer3D::GetRendererSettings().Deferred.MSAASampleCount << " samples; cell not run\n";
                    continue;
                }
                SetLateGeometry(late);
                Frames(kWarmFrames);

                // The serial is global: a frame left over from the previous cell
                // would satisfy "> 0" while this cell recorded nothing.
                const u64 serialBefore = RenderTargetCopyLedger::GetLastFrame().Serial;
                auto& tracker = RendererMemoryTracker::GetInstance();
                tracker.BeginPeakWindow();
                std::map<std::string, std::vector<f64>> passMs;
                std::vector<f64> frameMs;
                for (u32 frame = 0; frame < kMeasureFrames; ++frame)
                {
                    Frames(1);
                    const auto timings = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
                    if (timings.Frame.IsValid())
                        frameMs.push_back(timings.Frame.GpuMs);
                    for (const auto& pass : timings.Passes)
                        if (pass.IsValid())
                            passMs[pass.Name.ToStdString()].push_back(pass.Sample.GpuMs);
                }
                const RenderTargetCopyFrame copies = RenderTargetCopyLedger::GetLastFrame();
                const RendererMemoryReport memory = tracker.BuildReport();
                ASSERT_GT(copies.Serial, serialBefore) << "the ledger recorded no graph frame in this cell";

                nlohmann::json report;
                report["cell"] = name;
                report["width"] = kWidth;
                report["height"] = kHeight;
                report["lateGeometry"] = late;
                auto& rows = report["copies"] = nlohmann::json::array();
                std::map<std::string, std::pair<u32, u64>> byDestination;
                for (const auto& copy : copies.Copies)
                {
                    rows.push_back({ { "pass", copy.Pass.ToStdString() },
                                     { "kind", copy.IsBlit ? "blit" : "copy" },
                                     { "source", copy.Source.ToStdString() },
                                     { "destination", copy.Destination.ToStdString() },
                                     { "declared", copy.DestinationDeclared },
                                     { "width", copy.Width },
                                     { "height", copy.Height },
                                     { "sourceSamples", copy.SourceSamples },
                                     { "bytes", copy.Bytes ? nlohmann::json(*copy.Bytes) : nlohmann::json(nullptr) } });
                    auto& [count, bytes] = byDestination[copy.Destination.ToStdString()];
                    ++count;
                    bytes += copy.Bytes.value_or(0);

                    // A copy into a graph resource its pass never declared.
                    const bool graphResource = !copy.Destination.ToView().starts_with("<external>") &&
                                               !copy.Destination.ToView().starts_with("<default");
                    EXPECT_TRUE(!graphResource || copy.DestinationDeclared)
                        << copy.Pass.ToStdString() << " copies " << copy.Source.ToStdString() << " into "
                        << copy.Destination.ToStdString() << " without declaring the write";
                }
                report["copyCount"] = copies.Copies.Num();
                report["copyBytes"] = copies.KnownBytes();
                for (const auto& [destination, value] : byDestination)
                    report["byDestination"][destination] = { { "count", value.first }, { "bytes", value.second } };

                for (const auto& row : memory.Capacity)
                {
                    if (row.Owner.ToView() != "TransientPool")
                        continue;
                    report["transientPool"] = { { "capacityBytes", row.CapacityBytes.value_or(0) },
                                                { "demandBytes", row.ActiveDemandBytes.value_or(0) },
                                                { "aliasSavingsBytes", row.AliasSavingsBytes.value_or(0) } };
                }
                report["gpuResidentBytes"] = memory.Gpu.ResidentBytes();
                report["gpuWindowPeakBytes"] = memory.Gpu.WindowPeakBytes;
                report["frameGpuMsMedian"] = Median(frameMs);
                for (const auto& [pass, samples] : passMs)
                    report["passGpuMsMedian"][pass] = Median(samples);
                ASSERT_NO_FATAL_FAILURE(WriteArtefact(name, report));

                std::cout << "[ exports ] " << name << ": " << copies.Copies.Num() << " copies/blits, "
                          << copies.KnownBytes() << " bytes; frame " << Median(frameMs) << " ms; peak "
                          << memory.Gpu.WindowPeakBytes << "\n";
                for (const auto& copy : copies.Copies)
                {
                    std::cout << "[ exports ]   " << copy.Pass.ToStdString() << ": " << (copy.IsBlit ? "blit " : "copy ")
                              << copy.Source.ToStdString() << " -> " << copy.Destination.ToStdString() << " "
                              << copy.Bytes.value_or(0) << " B" << (copy.DestinationDeclared ? "" : " (undeclared)") << "\n";
                }
            }
        }
        SetLateGeometry(false);
    }

    namespace
    {
        [[nodiscard]] std::string_view BaseName(std::string_view name)
        {
            return name.substr(0, name.find('@'));
        }

        [[nodiscard]] RHI::ResourceHandle Resolve(const RenderGraph& graph, std::string_view name)
        {
            const RGTextureHandle handle = graph.GetTextureHandle(name);
            return handle.IsValid() ? graph.ResolveTextureHandle(handle) : RHI::ResourceHandle{};
        }

        struct ExportAttachment
        {
            std::string_view Export;
            RHI::ResourceHandle Attachment;
        };

        // The attachments the three exports name on the active path.
        [[nodiscard]] std::vector<ExportAttachment> ExportAttachments(const RenderGraph& graph, const bool deferred)
        {
            const Ref<Framebuffer> fb = graph.ResolveFramebuffer(graph.GetFramebufferHandle(deferred ? "GBufferResolved" : "SceneColor"));
            if (!fb)
                return {};
            return { { "SceneDepth", fb->GetDepthAttachmentHandle() },
                     { "SceneNormals", fb->GetColorAttachmentHandle(deferred ? 1u : 2u) },
                     { "Velocity", fb->GetColorAttachmentHandle(3u) } };
        }

        // AC2 at the declaration level, on the frame the graph just ran. For every
        // pass that reads an export, either the export IS the attachment (a view)
        // or the last copy into it was issued after the last pass that wrote the
        // attachment. Returns one line per stale read.
        [[nodiscard]] std::vector<std::string> FindStaleExportReads(const RenderGraph& graph, const RenderTargetCopyFrame& copies,
                                                                    const bool deferred)
        {
            std::vector<std::string> stale;
            const auto order = graph.GetExecutionOrder();
            const auto culled = graph.GetCulledPasses();
            const auto ran = [&culled](const FString& pass)
            { return std::ranges::find(culled, pass) == culled.end(); };
            // A write names the attachment through a view of it or through the
            // whole framebuffer: a render-target write covers every attachment.
            const auto writes = [&graph](const FString& pass, const RHI::ResourceHandle attachment)
            {
                const auto* accesses = graph.GetDeclaredPassAccesses(pass.ToView());
                const auto names = [&](const RGAccessDeclaration& access)
                {
                    if (Resolve(graph, access.ResourceName.ToView()) == attachment)
                        return true;
                    const RGFramebufferHandle handle = graph.GetFramebufferHandle(access.ResourceName.ToView());
                    const Ref<Framebuffer> fb = handle.IsValid() ? graph.ResolveFramebuffer(handle) : nullptr;
                    if (!fb)
                        return false;
                    if (fb->GetDepthAttachmentHandle() == attachment)
                        return true;
                    u32 color = 0;
                    for (const auto& spec : fb->GetSpecification().Attachments.Attachments)
                    {
                        if (spec.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                            spec.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F)
                            continue;
                        if (fb->GetColorAttachmentHandle(color++) == attachment)
                            return true;
                    }
                    return false;
                };
                return accesses && std::ranges::any_of(*accesses, [&](const RGAccessDeclaration& access)
                                                       { return access.IsWrite && names(access); });
            };

            for (const auto& [exportName, attachment] : ExportAttachments(graph, deferred))
            {
                if (!attachment.IsValid())
                    continue;
                const bool isView = Resolve(graph, exportName) == attachment;
                for (sizet consumer = 0; consumer < order.size(); ++consumer)
                {
                    if (!ran(order[consumer]))
                        continue;
                    const auto* accesses = graph.GetDeclaredPassAccesses(order[consumer].ToView());
                    if (!accesses || std::ranges::none_of(*accesses, [&](const RGAccessDeclaration& access)
                                                          { return !access.IsWrite && BaseName(access.ResourceName.ToView()) == exportName; }))
                        continue;
                    if (isView)
                        continue; // the consumer samples the attachment itself

                    // The copy model: find the refresh this consumer sees.
                    std::ptrdiff_t lastCopy = -1;
                    for (const auto& copy : copies.Copies)
                    {
                        if (BaseName(copy.Destination.ToView()) != exportName)
                            continue;
                        const auto it = std::ranges::find(order, copy.Pass);
                        const auto index = it == order.end() ? -1 : std::distance(order.begin(), it);
                        if (index <= static_cast<std::ptrdiff_t>(consumer))
                            lastCopy = std::max(lastCopy, index);
                    }
                    if (lastCopy < 0)
                    {
                        stale.push_back(order[consumer].ToStdString() + " reads " + std::string(exportName) + ", which nothing copied this frame");
                        continue;
                    }
                    for (sizet writer = static_cast<sizet>(lastCopy) + 1; writer < consumer; ++writer)
                    {
                        if (ran(order[writer]) && writes(order[writer], attachment))
                        {
                            stale.push_back(order[consumer].ToStdString() + " reads " + std::string(exportName) + " as " +
                                            order[static_cast<sizet>(lastCopy)].ToStdString() + " copied it, but " +
                                            order[writer].ToStdString() + " wrote the attachment after that copy");
                            break;
                        }
                    }
                }
            }
            return stale;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // AC2: every consumer of SceneDepth, SceneNormals and Velocity reads the
    // attachment as the last writer before it left it. With late geometry on
    // every path. Fails on the copy model (#1332's before state): the exports are
    // copied by ScenePass, the GPU-driven batches and foliage, and the passes
    // that draw after the last copy -- groom, decals, water, fluid, particles --
    // leave every later reader a version from before them.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, EveryExportConsumerReadsTheLastWriteOfItsAttachment)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedLever ledger(&Levers::RenderGraphCopyLedger, &Levers::SetRenderGraphCopyLedger, true);
        auto& post = Renderer3D::GetPostProcessSettings();
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        post.TAAEnabled = true;
        post.MotionBlurEnabled = true;
        post.Upscale = UpscaleMode::Off;

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(static_cast<int>(path));
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
            Renderer3D::ApplyRendererSettings();
            SetLateGeometry(true);
            Frames(kWarmFrames);

            const RenderGraph* graph = RenderGraphDebugRuntime::GetActiveGraph().Raw();
            ASSERT_NE(graph, nullptr);
            const auto stale = FindStaleExportReads(*graph, RenderTargetCopyLedger::GetLastFrame(), path == RenderingPath::Deferred);
            for (const auto& line : stale)
                ADD_FAILURE() << line;
            std::cout << "[ exports ] path " << static_cast<int>(path) << ": " << stale.size() << " stale export reads\n";

            // And the identity that makes it so: each export resolves to its attachment.
            for (const auto& [exportName, attachment] : ExportAttachments(*graph, path == RenderingPath::Deferred))
                EXPECT_EQ(Resolve(*graph, exportName), attachment) << exportName << " is not the attachment it names";
        }
        SetLateGeometry(false);
    }

    // -------------------------------------------------------------------------
    // The soft-particle fade samples the depth snapshot, because particles draw
    // into SceneColor, whose depth SceneDepth is on Forward. The snapshot is a
    // full-screen depth copy, so it is made only while a soft system draws:
    // the Scene installs a particle callback on every frame, and declaring the
    // read whenever the callback existed made the copy on every Forward frame,
    // particles or not. Pinned in both directions, and back again, and in
    // pixels: the soft frame fades the billboards where they cut the cubes and
    // leaves the rest, which it can do only by sampling a real scene depth.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, OnlyASoftParticleSystemHasTheDepthSnapshotMade)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedLever ledger(&Levers::RenderGraphCopyLedger, &Levers::SetRenderGraphCopyLedger, true);
        auto& post = Renderer3D::GetPostProcessSettings();
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        post.Upscale = UpscaleMode::Off;
        post.TAAEnabled = false;
        post.MotionBlurEnabled = false;
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::GetRendererSettings().OITEnabled = false;
        Renderer3D::ApplyRendererSettings();
        SetLateGeometry(false);

        const auto capture = [this](std::vector<u8>& frame)
        {
            Frames(kWarmFrames);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(frame, w, h));
        };
        const auto differing = [](const std::vector<u8>& a, const std::vector<u8>& b)
        {
            sizet count = 0;
            for (sizet i = 0; i + 3 < std::min(a.size(), b.size()); i += 4)
            {
                const i32 delta = std::max({ std::abs(a[i] - b[i]), std::abs(a[i + 1] - b[i + 1]), std::abs(a[i + 2] - b[i + 2]) });
                count += delta > 6 ? 1u : 0u;
            }
            return count;
        };
        std::vector<u8> empty;
        capture(empty);

        const auto snapshotCopies = []
        {
            const RenderTargetCopyFrame frame = RenderTargetCopyLedger::GetLastFrame();
            return std::ranges::count_if(frame.Copies, [](const RenderTargetCopyRecord& copy)
                                         { return copy.Destination.ToView() == ResourceNames::SceneDepthSnapshot; });
        };

        Entity emitter = GetScene().CreateEntity("SoftParticles");
        auto& component = emitter.AddComponent<ParticleSystemComponent>();
        auto& system = component.System;
        system.RenderMode = ParticleRenderMode::Billboard;
        system.GravityModule.Enabled = false;
        system.DragModule.Enabled = false;
        system.Emitter.RateOverTime = 0;
        system.SoftParticlesEnabled = false;
        system.SoftParticleDistance = 0.6f;
        auto& pool = system.GetPool();
        constexpr u32 kCount = 25;
        ASSERT_EQ(pool.Emit(kCount), kCount);
        for (u32 i = 0; i < kCount; ++i)
        {
            // A sheet through the cubes' centre plane: some billboards cut a
            // cube, the rest stand clear of everything behind them.
            pool.m_Positions[i] = pool.m_PrevPositions[i] = { 128.0f + 2.5f * static_cast<f32>(i % 5), 6.0f + 1.0f * static_cast<f32>(i / 5), 136.0f };
            pool.m_Velocities[i] = glm::vec3(0.0f);
            pool.m_Colors[i] = pool.m_InitialColors[i] = { 0.9f, 0.9f, 1.0f, 0.6f };
            pool.m_Sizes[i] = pool.m_PrevSizes[i] = pool.m_InitialSizes[i] = 2.0f;
            pool.m_Lifetimes[i] = pool.m_MaxLifetimes[i] = 1000.0f;
        }
        system.Update(0.001f, glm::vec3(0.0f));

        std::vector<u8> hard;
        capture(hard);
        EXPECT_EQ(snapshotCopies(), 0) << "hard particles made the depth snapshot: 8.3 MB copied for no reader";

        system.SoftParticlesEnabled = true;
        std::vector<u8> soft;
        capture(soft);
        EXPECT_EQ(snapshotCopies(), 1) << "a soft system drew without the depth snapshot it fades against";
        const sizet covered = differing(empty, hard);
        const sizet faded = differing(hard, soft);
        std::cout << "[ exports ] soft particles: " << covered << " particle pixels, " << faded << " faded\n";
        ASSERT_GT(covered, 2000u) << "the particle sheet is not in view, so the fade below proves nothing";
        EXPECT_GT(faded, 0u) << "soft particles drew exactly like hard ones: the fade never sampled a depth";
        EXPECT_LT(faded, covered / 2) << "the fade reached most of the sheet: the depth it sampled is not the scene's";
        const RenderGraph* graph = RenderGraphDebugRuntime::GetActiveGraph().Raw();
        ASSERT_NE(graph, nullptr);
        const auto* accesses = graph->GetDeclaredPassAccesses("ParticlePass");
        ASSERT_NE(accesses, nullptr) << "ParticlePass declared nothing";
        EXPECT_TRUE(std::ranges::any_of(*accesses, [](const RGAccessDeclaration& access)
                                        { return !access.IsWrite && access.ResourceName.ToView() == ResourceNames::SceneDepthSnapshot; }))
            << "ParticlePass samples the snapshot without declaring it, so nothing orders the copy before it";

        system.SoftParticlesEnabled = false;
        Frames(kWarmFrames);
        EXPECT_EQ(snapshotCopies(), 0) << "the snapshot outlived the last soft system";

        GetScene().DestroyEntity(emitter);
    }

    // -------------------------------------------------------------------------
    // AC2 in pixels: what the temporal resolve samples as Velocity is the scene
    // target's velocity attachment AS TAA RUNS, bit for bit, with the camera
    // moving and the GPU-driven instanced batches drawn after ScenePass. On the
    // copy model that pass re-copied depth and normals but never velocity, so
    // TAA reprojected every batch with the motion of what lay behind it.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, TemporalResolveSamplesTheVelocityTheLateGeometryWrote)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& post = Renderer3D::GetPostProcessSettings();
        post.TAAEnabled = true;
        post.Upscale = UpscaleMode::Off;
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        // No foliage: its colour pass re-copied velocity on the copy model, which
        // would hide the GPU-driven batches' missing refresh this is about.
        SetLateGeometry(true, /*water*/ true, /*foliagePresent*/ false);

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        for (u32 frame = 0; frame < kWarmFrames; ++frame)
        {
            camera.SetPose(kEye + glm::vec3(0.25f * static_cast<f32>(frame), 0.0f, 0.0f), 0.0f, 0.06f);
            RunEditorFrames(camera, 1);
        }

        RenderGraph* graph = const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
        ASSERT_NE(graph, nullptr);
        std::vector<f32> sampled;
        std::vector<f32> attachment;
        constexpr std::string_view kHook = "RenderTargetExportVelocity";
        graph->AddPostPassHook(kHook, [&sampled, &attachment](std::string_view pass, RenderGraph& g)
                               {
                                   if (pass != "TAAPass")
                                       return;
                                   const u32 velocity = g.ResolveTexture(g.GetTextureHandle("Velocity"));
                                   const Ref<Framebuffer> scene = g.ResolveFramebuffer(g.GetFramebufferHandle("SceneColor"));
                                   if (velocity != 0u && scene)
                                   {
                                       ReadbackRgbaFloat(velocity, kWidth, kHeight, sampled);
                                       ReadbackRgbaFloat(scene->GetColorAttachmentRendererID(3), kWidth, kHeight, attachment);
                                   } });
        camera.SetPose(kEye + glm::vec3(0.25f * static_cast<f32>(kWarmFrames) + 1.0f, 0.0f, 0.0f), 0.0f, 0.06f);
        RunEditorFrames(camera, 1);
        graph->RemovePostPassHook(kHook);

        ASSERT_FALSE(sampled.empty()) << "the hook never saw TAAPass";
        ASSERT_EQ(sampled.size(), attachment.size());
        sizet moving = 0;
        sizet differing = 0;
        for (sizet i = 0; i + 3 < sampled.size(); i += 4)
        {
            moving += std::abs(attachment[i]) + std::abs(attachment[i + 1]) > 1e-5f ? 1u : 0u;
            differing += (!Math::BitwiseEqual(sampled[i], attachment[i]) ||
                          !Math::BitwiseEqual(sampled[i + 1], attachment[i + 1]))
                             ? 1u
                             : 0u;
        }
        std::cout << "[ exports ] TAA velocity: " << moving << " moving texels, " << differing << " differ from the attachment\n";
        EXPECT_GT(moving, kWidth * kHeight / 100u) << "the camera moved but the attachment holds no motion: the check is vacuous";
        EXPECT_EQ(differing, 0u) << "TAA sampled a velocity that is not the scene target's as TAA ran";
        SetLateGeometry(false);
    }

    // -------------------------------------------------------------------------
    // AC3: the migrated exports are views of a pooled framebuffer. With transient
    // aliasing off, and with a capture snapshot armed on them, the frame must be
    // the same frame: views follow their parent's lifetime, and a capture clone
    // is a separate copy, not a second identity the frame depends on. Static
    // content (no water, no wind, a frozen clock, no temporal resolve) so the
    // depth readback is exact and the composite differs only by the AO noise the
    // repeat measures.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, AliasingAndCaptureLeaveTheExportsAndTheFrameUnchanged)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct MockClock
        {
            MockClock()
            {
                Time::SetMockTime(7.0f);
            }
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        auto& post = Renderer3D::GetPostProcessSettings();
        post.TAAEnabled = false;
        post.MotionBlurEnabled = false;
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        post.Upscale = UpscaleMode::Off;
        Renderer3D::GetWindSettings().Enabled = false;

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::Deferred })
        {
            SCOPED_TRACE(static_cast<int>(path));
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
            SetLateGeometry(true, /*water*/ false);

            const auto capture = [this](std::vector<u8>& frame, std::vector<f32>& depth)
            {
                Frames(kWarmFrames);
                u32 w = 0;
                u32 h = 0;
                ASSERT_TRUE(ReadbackComposite(frame, w, h));
                const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SceneDepth);
                ASSERT_NE(texture, 0u);
                depth.assign(static_cast<sizet>(kWidth) * kHeight, 0.0f);
                glGetTextureImage(texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT, static_cast<GLsizei>(depth.size() * sizeof(f32)),
                                  depth.data());
            };

            std::vector<u8> baseFrame;
            std::vector<u8> repeatFrame;
            std::vector<f32> baseDepth;
            std::vector<f32> repeatDepth;
            capture(baseFrame, baseDepth);
            capture(repeatFrame, repeatDepth);
            const f64 noise = VisualEvidence::Rgba8Rmse(baseFrame, repeatFrame);

            std::vector<u8> unaliasedFrame;
            std::vector<f32> unaliasedDepth;
            {
                const ScopedLever noAliasing(&Levers::DisableTransientAliasing, &Levers::SetDisableTransientAliasing, true);
                capture(unaliasedFrame, unaliasedDepth);
            }

            // Capture on: clone the three exports after the last post pass that reads them.
            RenderGraph* graph = const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
            ASSERT_NE(graph, nullptr);
            RenderGraphPassSnapshot snapshot;
            TDoubleLinkedList<RenderGraphPassSnapshot::Request> requests;
            for (const std::string_view name : { "SceneDepth", "SceneNormals", "Velocity" })
            {
                requests.AddTail(RenderGraphPassSnapshot::Request(name, [graph, name]()
                                                                  { return graph->ResolveTextureHandle(graph->GetTextureHandle(name)); }));
            }
            snapshot.Arm(graph, "ToneMapPass", std::move(requests));
            std::vector<u8> capturedFrame;
            std::vector<f32> capturedDepth;
            capture(capturedFrame, capturedDepth);
            sizet clones = 0;
            for (const auto& result : snapshot.GetResults())
                clones += result.Captured && result.Handle.IsValid() && result.Handle != result.SourceHandle ? 1u : 0u;
            snapshot.Disarm();

            EXPECT_EQ(clones, 3u) << "the capture did not clone the three exports into storage of its own";
            const f64 aliasRmse = VisualEvidence::Rgba8Rmse(baseFrame, unaliasedFrame);
            const f64 captureRmse = VisualEvidence::Rgba8Rmse(baseFrame, capturedFrame);
            const f64 allowance = std::max(2.0 * noise, 0.25);
            std::cout << "[ exports ] path " << static_cast<int>(path) << ": repeat rmse " << noise << ", aliasing off rmse "
                      << aliasRmse << ", capture on rmse " << captureRmse << "\n";
            EXPECT_EQ(baseDepth, repeatDepth) << "the depth is not deterministic: the comparisons below would be vacuous";
            EXPECT_EQ(baseDepth, unaliasedDepth) << "the scene depth depends on transient aliasing";
            EXPECT_EQ(baseDepth, capturedDepth) << "the scene depth depends on a capture being armed";
            EXPECT_LE(aliasRmse, allowance) << "turning transient aliasing off changed the frame: a view outlived its storage";
            EXPECT_LE(captureRmse, allowance) << "arming a capture changed the frame";
        }
        SetLateGeometry(false);
    }

    // -------------------------------------------------------------------------
    // The visual evidence: every reachable GL cell, late geometry in view, from
    // two angles. GraphOwnedExports_GL_<cell>_<angle>.png is this tree;
    // GraphOwnedExportsOff_GL_<cell>_<angle>.png is the same test run on the copy
    // model, committed beside it as the A/B control.
    // -------------------------------------------------------------------------
    TEST_F(RenderTargetExportEvidence, EveryCellRendersTheLateGeometryFromTwoAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct Cell
        {
            const char* Name;
            RenderingPath Path;
            u32 Msaa;
            UpscaleMode Upscale;
        };
        // MSAA reaches the Deferred G-Buffer only: the forward scene target is
        // single-sample (Renderer3DRenderGraphSetup.cpp), so there is no forward
        // MSAA cell to run.
        static constexpr std::array<Cell, 7> kCells = { {
            { "Forward", RenderingPath::Forward, 1u, UpscaleMode::Off },
            { "ForwardPlus", RenderingPath::ForwardPlus, 1u, UpscaleMode::Off },
            { "Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off },
            { "Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off },
            { "Forward_Upscale", RenderingPath::Forward, 1u, UpscaleMode::Performance },
            { "ForwardPlus_Upscale", RenderingPath::ForwardPlus, 1u, UpscaleMode::Performance },
            { "Deferred_Upscale", RenderingPath::Deferred, 1u, UpscaleMode::Performance },
        } };
        struct MockClock
        {
            MockClock()
            {
                Time::SetMockTime(7.0f);
            }
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        auto& post = Renderer3D::GetPostProcessSettings();
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        post.TAAEnabled = true;
        SetLateGeometry(true);

        for (const Cell& cell : kCells)
        {
            SCOPED_TRACE(cell.Name);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = cell.Path;
            settings.Deferred.MSAASampleCount = cell.Msaa;
            post.Upscale = cell.Upscale;
            Renderer3D::ApplyRendererSettings();
            if (Renderer3D::GetRendererSettings().Deferred.MSAASampleCount != cell.Msaa)
                continue;
            std::vector<std::vector<u8>> captures;
            for (const auto& [angle, eye, yaw, pitch] : { std::tuple{ "Front", kEye, 0.0f, 0.06f },
                                                          std::tuple{ "Oblique", glm::vec3(146.0f, 18.0f, 150.0f), 0.45f, 0.25f } })
            {
                EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
                camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
                camera.SetPose(eye, yaw, pitch);
                RunEditorFrames(camera, kWarmFrames);
                std::vector<u8> frame;
                u32 w = 0;
                u32 h = 0;
                ASSERT_TRUE(ReadbackComposite(frame, w, h));
                VisualEvidence::FlipRgbaRowsInPlace(frame, w, h);
                // The red decal and the green-leaved pines are the late geometry.
                VisualEvidence::ExpectFrameHasSubject(frame, angle, [](u32 r, u32 g, u32 b)
                                                      { return g > 12u && g > r && g > b; });
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string file = "GraphOwnedExports_GL_" + std::string(cell.Name) + "_" + angle + ".png";
                EXPECT_NE(::stbi_write_png((dir / file).string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                           frame.data(), static_cast<int>(w) * 4),
                          0);
                captures.push_back(std::move(frame));
            }
            VisualEvidence::ExpectCapturesAreDistinct(captures, { "Front", "Oblique" }, 1.0);
        }
        post.Upscale = UpscaleMode::Off;
        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
        SetLateGeometry(false);
    }
} // namespace OloEngine::Tests
