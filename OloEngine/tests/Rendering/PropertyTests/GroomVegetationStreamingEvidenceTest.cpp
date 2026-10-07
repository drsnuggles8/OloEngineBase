// OLO_TEST_LAYER: L8
// Full Scene::OnUpdateRuntime evidence for #1257. CPU contract tests cover the
// load/budget state machine; this fixture proves that both consumers still draw
// while detail is gated, under pressure, and after the same identities reload.
// GL only; Vulkan, cooked packs, RT and physical retirement require live evidence.

#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "VisualEvidenceGuards.h"
#include "TestAsyncLoadHooks.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    class GroomVegetationStreamingEvidenceTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kWidth = 960;
        static constexpr u32 kHeight = 540;
        static constexpr u32 kStrands = 2048;

        void BuildScene() override
        {
            EnsureTaskSchedulerStarted();
            RepresentationStreaming::Get().Reset();
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                const auto directory = TempDir("streaming-project");
                std::filesystem::create_directories(directory / "Assets");
                std::ofstream project(directory / "Evidence.oloproj");
                project << "Project:\n  Name: StreamingEvidence\n  StartScene: \"\"\n"
                           "  AssetDirectory: Assets\n  ScriptModulePath: \"\"\n";
                project.close();
                ASSERT_TRUE(Project::Load(directory / "Evidence.oloproj"));
                auto manager = Ref<EditorAssetManager>::Create();
                manager->Initialize(false);
                Project::SetAssetManager(manager);
            }
            EnableRendering(kWidth, kHeight);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowComponentGizmos = false;
            Renderer3D::GetPostProcessSettings().TAAEnabled = false;

            Scene& scene = GetScene();
            scene.GetStreamingSettings().RepresentationResidentMegabytes = 64.0f;
            scene.GetStreamingSettings().RepresentationUploadMegabytesPerFrame = 8.0f;
            scene.GetStreamingSettings().RepresentationStagingMegabytes = 64.0f;
            m_Camera = scene.CreateEntity("Runtime Camera");
            auto& camera = m_Camera.AddComponent<CameraComponent>();
            camera.Primary = true;
            camera.Camera.SetPerspective(glm::radians(55.0f), 0.05f, 300.0f);
            camera.Camera.SetViewportSize(kWidth, kHeight);
            Pose({ 0.0f, 4.0f, 14.0f });

            auto sun = scene.CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.5f));
            light.m_Intensity = 4.0f;

            InstallGroom();
            ASSERT_FALSE(HasFatalFailure());
            m_Terrain = scene.CreateEntity("Plants");
            m_Terrain.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -4.0f };
            auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 1257;
            terrain.m_ProceduralResolution = 32;
            terrain.m_WorldSizeX = 8.0f;
            terrain.m_WorldSizeZ = 8.0f;
            terrain.m_HeightScale = 0.1f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);
            auto& foliage = m_Terrain.AddComponent<FoliageComponent>();
            FoliageLayer plants;
            plants.Name = "Streaming Pines";
            plants.MeshPath = "SandboxProject/Assets/Models/Vegetation/pine.obj";
            plants.AlbedoPath = "assets/textures/grass.png";
            plants.Density = 0.5f;
            plants.SplatmapChannel = -1;
            plants.MinSlopeAngle = 0.0f;
            plants.MaxSlopeAngle = 90.0f;
            plants.MinScale = 1.0f;
            plants.MaxScale = 1.0f;
            plants.MinHeight = 2.5f;
            plants.MaxHeight = 2.5f;
            plants.ViewDistance = 250.0f;
            plants.FadeStartDistance = 220.0f;
            plants.UseAuthoredMesh = true;
            plants.MeshViewDistance = 100.0f;
            plants.MeshFadeStartDistance = 90.0f;
            plants.AlphaCutoff = 0.2f;
            plants.WindStrength = 0.0f;
            plants.BaseColor = { 0.12f, 0.7f, 0.1f };
            foliage.m_Layers.Add(plants);
            foliage.m_Renderer = Ref<FoliageRenderer>::Create();
            foliage.m_Renderer->SetStreamingLoadGate(m_Gate);
            ASSERT_NE(Renderer3D::GetGroomRenderPass(), nullptr);
            Renderer3D::GetGroomRenderPass()->SetStreamingStartGate(m_Gate);
            // RendererAttachedTest drives the real OnUpdateRuntime render path
            // without an Application. OnRuntimeStart boots the application-owned
            // gameplay services and unconditionally reads its random seed; that
            // lifecycle is outside this renderer-only host.
        }

        void InstallGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            ASSERT_TRUE(builder.AddGroup("coat", group, reason)) << reason;
            for (u32 strand = 0; strand < kStrands; ++strand)
            {
                const f32 v = (static_cast<f32>(strand) + 0.5f) / static_cast<f32>(kStrands);
                const f32 phi = 2.39996323f * static_cast<f32>(strand);
                const f32 y = 1.0f - 2.0f * v;
                const f32 radial = std::sqrt(std::max(0.0f, 1.0f - y * y));
                const glm::vec3 normal(radial * std::cos(phi), y, radial * std::sin(phi));
                std::array<glm::vec3, 4> points;
                std::array<f32, 4> widths;
                for (u32 point = 0; point < 4; ++point)
                {
                    const f32 along = static_cast<f32>(point) / 3.0f;
                    points[point] = normal * (1.0f + 0.6f * along);
                    widths[point] = 0.04f * (1.0f - 0.7f * along);
                }
                const f32 u = phi / 6.28318531f;
                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { u - std::floor(u), v };
                input.GroupId = group;
                ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
            }
            m_Groom = builder.Build(reason);
            ASSERT_TRUE(m_Groom) << reason;
            ASSERT_TRUE(GroomCooker::Canonicalize(*m_Groom, reason)) << reason;
            GroomCardSettings cards;
            cards.CellSize = 0.1f;
            cards.SourcePixelSize = 180.0f;
            GroomLodLevel level;
            ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*m_Groom, cards, level, reason)) << reason;
            ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*m_Groom, { std::move(level) }, reason)) << reason;
            m_GroomEntity = GetScene().CreateEntity("Streaming Coat");
            m_GroomEntity.GetComponent<TransformComponent>().Translation = { -3.0f, 2.0f, 0.0f };
            auto& groom = m_GroomEntity.AddComponent<GroomComponent>();
            groom.m_Groom = AssetManager::AddMemoryOnlyAsset<GroomAsset>(m_Groom);
            groom.m_ShowPreview = false;
            groom.m_RenderStrands = true;
            groom.m_MaxRenderStrands = kStrands;
            groom.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groom.m_StrandColor = { 0.95f, 0.35f, 0.08f };
        }

        void TearDown() override
        {
            m_Gate.Trigger(); // also releases workers after an ASSERT returns early
            if (RenderPropertyFixture::IsGpuAvailable())
            {
                if (auto* pass = Renderer3D::GetGroomRenderPass())
                    pass->SetStreamingStartGate(std::nullopt);
                if (m_Terrain && m_Terrain.GetComponent<FoliageComponent>().m_Renderer)
                    m_Terrain.GetComponent<FoliageComponent>().m_Renderer->SetStreamingLoadGate(std::nullopt);
            }
            RendererAttachedTest::TearDown();
            RepresentationStreaming::Get().Reset();
        }

        void Pose(const glm::vec3& eye)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = eye;
            transform.SetRotationEuler({ -0.12f, std::atan2(eye.x, eye.z), 0.0f });
        }

        [[nodiscard]] FoliageRenderer& Plants()
        {
            return *m_Terrain.GetComponent<FoliageComponent>().m_Renderer;
        }

        [[nodiscard]] static u64 FoliageDetailLiveBytes()
        {
            u64 bytes = 0;
            for (const auto& owner : RendererMemoryTracker::GetInstance().BuildReport().Owners)
                if (owner.Owner.ToView() == "Foliage streaming detail")
                    bytes += owner.GpuLiveBytes;
            return bytes;
        }

        void ExpectPlantsDisabled()
        {
            const auto& stats = Plants().GetStreamingStats();
            EXPECT_EQ(stats.OptionalGpuBytes, 0u);
            EXPECT_EQ(stats.ResidentLayers, 0u);
            EXPECT_EQ(stats.PendingLayers, 0u);
            EXPECT_EQ(stats.PreparedCpuBytes, 0u);
            EXPECT_EQ(Plants().GetTotalInstanceCount(), 0u);
            EXPECT_TRUE(Plants().GetInstanceRegistry().GetRecords().IsEmpty());
            EXPECT_TRUE(Plants().GetActiveLayerDrawInfo().IsEmpty())
                << "disabled foliage must offer neither card nor mesh draws to the scene";
            EXPECT_GT(stats.PinnedGpuBytes, 0u) << "disabling detail retains the reusable drawable floor";
        }

        [[nodiscard]] bool WaitForDetail()
        {
            const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
            while (std::chrono::steady_clock::now() < deadline)
            {
                RunFrames(1);
                if (Renderer3D::GetGroomRenderPass()->GetStreamingStats().DetailDraws > 0 &&
                    Plants().GetStreamingStats().ResidentLayers > 0)
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        template<typename Predicate>
        [[nodiscard]] bool WaitForPlants(const Predicate& predicate)
        {
            const auto deadline = std::chrono::steady_clock::now() + kLoadHookFailAfter;
            while (std::chrono::steady_clock::now() < deadline)
            {
                RunFrames(1);
                if (predicate(Plants().GetStreamingStats()))
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        std::vector<u8> Capture(const std::string& name)
        {
            std::vector<u8> pixels;
            u32 width = 0;
            u32 height = 0;
            EXPECT_TRUE(ReadbackComposite(pixels, width, height));
            EXPECT_EQ(width, kWidth);
            EXPECT_EQ(height, kHeight);
            VisualEvidence::ExpectFrameHasSubject(pixels, name + " coat", [](u32 r, u32 g, u32 b)
                                                  { return r > 90 && r > g * 1.2 && r > b * 1.5; }, 0.001);
            VisualEvidence::ExpectFrameHasSubject(pixels, name + " plants", [](u32 r, u32 g, u32 b)
                                                  { return g > 50 && g > r * 1.15 && g > b * 1.15; }, 0.001);
            if (!pixels.empty())
            {
                auto flipped = pixels;
                VisualEvidence::FlipRgbaRowsInPlace(flipped, width, height);
                const std::filesystem::path directory = "assets/tests/visual";
                std::filesystem::create_directories(directory);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(),
                                         static_cast<int>(width), static_cast<int>(height), 4,
                                         flipped.data(), static_cast<int>(width * 4u)),
                          0);
            }
            return pixels;
        }

        Entity m_Camera;
        Entity m_GroomEntity;
        Entity m_Terrain;
        Ref<GroomAsset> m_Groom;
        Tasks::FTaskEvent m_Gate{ "GroomVegetationEvidence" };
    };

    TEST_F(GroomVegetationStreamingEvidenceTest, EagerLayerWithoutPlacementsReleasesItsDetailUnderPressure)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        m_GroomEntity.RemoveComponent<GroomComponent>();
        auto& foliage = m_Terrain.GetComponent<FoliageComponent>();
        auto rejected = foliage.m_Layers[0];
        rejected.Name = "Uninhabitable Pines";
        rejected.UseAltitudeBand = true;
        rejected.MinAltitude = 1000.0f;
        rejected.MaxAltitude = 1001.0f;
        foliage.m_Layers.Add(rejected);
        auto& settings = GetScene().GetStreamingSettings();
        settings.RepresentationResidentMegabytes = 0.0f;
        settings.RepresentationUploadMegabytesPerFrame = 0.0f;
        settings.RepresentationStagingMegabytes = 0.0f;
        RunFrames(6);
        const auto plantId = m_Terrain.GetUUID();
        const auto instanceCount = Plants().GetTotalInstanceCount();
        ASSERT_GT(instanceCount, 0u);
        ASSERT_EQ(Plants().GetStreamingStats().ResidentLayers, 2u)
            << "eager loading constructs both private meshes before rejecting habitat placements";
        ASSERT_GT(Plants().GetStreamingStats().OptionalGpuBytes, 0u);

        settings.RepresentationResidentMegabytes = 1.0f / (1024.0f * 1024.0f);
        settings.RepresentationUploadMegabytesPerFrame = 8.0f;
        settings.RepresentationStagingMegabytes = 64.0f;
        RunFrames(1);
        EXPECT_EQ(Plants().GetStreamingStats().ResidentLayers, 0u);
        EXPECT_EQ(Plants().GetStreamingStats().OptionalGpuBytes, 0u);
        EXPECT_EQ(FoliageDetailLiveBytes(), 0u)
            << "the zero-instance layer has no residency key; its actual private holders must still be released";
        EXPECT_GT(Plants().GetStreamingStats().Evictions, 0u);
        RunFrames(6);
        EXPECT_EQ(RendererMemoryTracker::GetInstance().GetOwnerGpuRetiringBytes("Foliage streaming detail"), 0u);
        EXPECT_EQ(RepresentationStreaming::Get().GetStats().OptionalResidentGpuBytes, 0u);
        EXPECT_GT(Plants().GetStreamingStats().PinnedGpuBytes, 0u);
        EXPECT_EQ(Plants().GetTotalInstanceCount(), instanceCount);
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);
        EXPECT_FALSE(Plants().GetActiveLayerDrawInfo().IsEmpty());
        for (const auto& draw : Plants().GetActiveLayerDrawInfo())
            EXPECT_FALSE(draw.IsAuthoredMesh);
    }

    TEST_F(GroomVegetationStreamingEvidenceTest, DisablingPendingPlantsReleasesTheirReservationAndReenableDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        // Keep both workers gated. Extra frames drain retirement from a preceding
        // fixture without letting either family prepare its optional geometry.
        RunFrames(6);
        const auto plantId = m_Terrain.GetUUID();
        const auto pending = Plants().GetStreamingStats();
        ASSERT_GT(pending.PendingLayers, 0u);
        ASSERT_GT(pending.PendingCpuBytes, 0u);
        ASSERT_GT(Plants().GetTotalInstanceCount(), 0u);
        ASSERT_GT(Renderer3D::GetGroomRenderPass()->GetStreamingStats().Pending, 0u);
        const u64 stagingBefore = RepresentationStreaming::Get().GetStats().StagingCpuBytes;
        ASSERT_GE(stagingBefore, pending.PendingCpuBytes);

        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = false;
        RunFrames(1); // production Scene disabled-component route invokes ClearInstances
        ExpectPlantsDisabled();
        EXPECT_EQ(Plants().GetStreamingStats().PendingCpuBytes, 0u)
            << "a cancelled-before-start plant must not retain a staging lease";
        EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes,
                  stagingBefore - pending.PendingCpuBytes)
            << "release only the plant reservation; the gated groom still owns its lease";
        EXPECT_GT(Renderer3D::GetGroomRenderPass()->GetStreamingStats().Pending, 0u);
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);

        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = true;
        RunFrames(1);
        EXPECT_GT(Plants().GetStreamingStats().PendingLayers, 0u);
        m_Gate.Trigger();
        ASSERT_TRUE(WaitForDetail());
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);
        EXPECT_GT(Plants().GetTotalInstanceCount(), 0u);
        EXPECT_FALSE(Plants().GetActiveLayerDrawInfo().IsEmpty());
        Capture("GroomVegetationStreaming_GL_PendingDisabled_Reenabled");
    }

    TEST_F(GroomVegetationStreamingEvidenceTest, DisablingResidentPlantsDropsDrawHandlesButChargesPhysicalRetirement)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        RunFrames(2);
        m_Gate.Trigger();
        ASSERT_TRUE(WaitForDetail());
        RunFrames(6); // settle cutoff texture replacements and previous-frame draw packets
        const auto plantId = m_Terrain.GetUUID();
        ASSERT_GT(Plants().GetStreamingStats().OptionalGpuBytes, 0u);
        auto& tracker = RendererMemoryTracker::GetInstance();
        const u64 liveBefore = FoliageDetailLiveBytes();
        ASSERT_GT(liveBefore, 0u);
        const u64 retiringBefore = tracker.GetOwnerGpuRetiringBytes("Foliage streaming detail");
        TArray<RHI::ResourceHandle> meshHandles;
        for (const auto& draw : Plants().GetActiveLayerDrawInfo())
            if (draw.IsAuthoredMesh)
                meshHandles.Add(draw.VertexArrayID);
        ASSERT_FALSE(meshHandles.IsEmpty());
        for (const auto handle : meshHandles)
            ASSERT_TRUE(RHI::ResourceRegistry::Get().IsLive(handle));

        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = false;
        RunFrames(1);
        ExpectPlantsDisabled();
        EXPECT_EQ(Plants().GetStreamingStats().PendingCpuBytes, 0u);
        EXPECT_EQ(FoliageDetailLiveBytes(), 0u)
            << "an optional VAO retaining its mesh buffers would keep detail physically live";
        for (const auto handle : meshHandles)
            EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(handle));
        const u64 retiringAfter = tracker.GetOwnerGpuRetiringBytes("Foliage streaming detail");
        EXPECT_GE(retiringAfter, retiringBefore + liveBefore)
            << "released mesh backing must stay charged until the GL deferred deletion queue drains";
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);

        RunFrames(6);
        EXPECT_EQ(tracker.GetOwnerGpuRetiringBytes("Foliage streaming detail"), 0u);
        ExpectPlantsDisabled();
        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = true;
        ASSERT_TRUE(WaitForDetail());
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);
        EXPECT_GT(Plants().GetStreamingStats().OptionalGpuBytes, 0u);
        EXPECT_GT(Plants().GetTotalInstanceCount(), 0u);
        Capture("GroomVegetationStreaming_GL_ResidentDisabled_Reenabled");
    }

    TEST_F(GroomVegetationStreamingEvidenceTest, DisablingPreparedPlantsReleasesThePayloadAndItsHeldStagingLease)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        RunFrames(2);
        ASSERT_GT(Plants().GetStreamingStats().PendingLayers, 0u);
        // This case measures a held CPU payload's lease, so cancel the other
        // family's request before allowing the workers to complete.
        m_GroomEntity.RemoveComponent<GroomComponent>();
        RunFrames(2);
        EXPECT_EQ(Renderer3D::GetGroomRenderPass()->GetStreamingStats().Pending, 0u);
        EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes,
                  Plants().GetStreamingStats().PendingCpuBytes);
        // A positive one-byte limit refuses even the smallest authored mesh.
        GetScene().GetStreamingSettings().RepresentationUploadMegabytesPerFrame = 0.000001f;
        m_Gate.Trigger();
        ASSERT_TRUE(WaitForPlants([](const FFoliageStreamingStats& stats)
                                  { return stats.PreparedCpuBytes > 0u; }))
            << "preparation must complete while the positive upload cap refuses integration";
        const auto prepared = Plants().GetStreamingStats();
        ASSERT_GT(prepared.PendingCpuBytes, 0u);
        ASSERT_EQ(prepared.OptionalGpuBytes, 0u);
        ASSERT_EQ(prepared.ResidentLayers, 0u);
        ASSERT_GT(Plants().GetTotalInstanceCount(), 0u);
        EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, prepared.PendingCpuBytes);
        const auto plantId = m_Terrain.GetUUID();

        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = false;
        RunFrames(1);
        ExpectPlantsDisabled();
        EXPECT_EQ(Plants().GetStreamingStats().PendingCpuBytes, 0u);
        EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, 0u)
            << "a disabled plant must release its completed payload and its retained staging lease";
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);

        GetScene().GetStreamingSettings().RepresentationUploadMegabytesPerFrame = 8.0f;
        m_Terrain.GetComponent<FoliageComponent>().m_Enabled = true;
        ASSERT_TRUE(WaitForPlants([](const FFoliageStreamingStats& stats)
                                  { return stats.ResidentLayers > 0u; }));
        EXPECT_EQ(m_Terrain.GetUUID(), plantId);
        EXPECT_GT(Plants().GetTotalInstanceCount(), 0u);
        EXPECT_FALSE(Plants().GetActiveLayerDrawInfo().IsEmpty());
    }

    TEST_F(GroomVegetationStreamingEvidenceTest, PendingPressureAndReloadStayDrawableOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const std::array<std::pair<RenderingPath, const char*>, 3> paths = { { { RenderingPath::Forward, "Forward" }, { RenderingPath::ForwardPlus, "ForwardPlus" }, { RenderingPath::Deferred, "Deferred" } } };
        for (const auto& [path, label] : paths)
        {
            const std::string stem = std::string("GroomVegetationStreaming_GL_") + label;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
            Pose({ 0.0f, 4.0f, 14.0f });
            RunFrames(2);
            if (path == RenderingPath::Forward)
            {
                EXPECT_GT(Renderer3D::GetGroomRenderPass()->GetStreamingStats().Pending, 0u);
                EXPECT_GT(Plants().GetStreamingStats().PendingLayers, 0u);
                Capture(stem + "_Pending");
                m_Gate.Trigger();
            }
            ASSERT_TRUE(WaitForDetail()) << label;
            const auto groomId = m_GroomEntity.GetUUID();
            const auto handle = m_GroomEntity.GetComponent<GroomComponent>().m_Groom;
            const auto roots = m_Groom->GetRootUVs();
            TArray<u64> ids;
            for (const auto& record : Plants().GetInstanceRegistry().GetRecords())
                ids.Add(record.m_Id);
            ASSERT_GT(ids.Num(), 0);
            Capture(stem + "_Detail");
            GetScene().GetStreamingSettings().RepresentationResidentMegabytes = 0.001f;
            RunFrames(3);
            EXPECT_GT(Renderer3D::GetGroomRenderPass()->GetStreamingStats().FallbackDraws, 0u);
            EXPECT_EQ(Plants().GetStreamingStats().ResidentLayers, 0u);
            EXPECT_GT(Plants().GetStreamingStats().FallbackLayers, 0u);
            Capture(stem + "_Pressure");
            GetScene().GetStreamingSettings().RepresentationResidentMegabytes = 64.0f;
            ASSERT_TRUE(WaitForDetail()) << label;
            EXPECT_EQ(m_GroomEntity.GetUUID(), groomId);
            EXPECT_EQ(m_GroomEntity.GetComponent<GroomComponent>().m_Groom, handle);
            ASSERT_EQ(m_Groom->GetRootUVs().size(), roots.size());
            for (sizet i = 0; i < roots.size(); ++i)
                EXPECT_TRUE(Math::BitwiseEqual(m_Groom->GetRootUVs()[i], roots[i]));
            const auto& records = Plants().GetInstanceRegistry().GetRecords();
            ASSERT_EQ(records.Num(), ids.Num());
            for (i32 i = 0; i < ids.Num(); ++i)
                EXPECT_EQ(records[i].m_Id, ids[i]);
            const auto front = Capture(stem + "_Reload");
            RunFrames(1);
            const auto repeat = Capture(stem + "_Repeat");
            Pose({ 12.0f, 4.0f, 10.0f });
            RunFrames(2);
            const auto oblique = Capture(stem + "_Oblique");
            Pose({ -10.0f, 4.0f, 12.0f });
            RunFrames(2);
            const auto reverse = Capture(stem + "_Reverse");
            VisualEvidence::ExpectCapturesAreDistinct({ front, oblique, reverse },
                                                      { "front", "oblique", "reverse" }, VisualEvidence::Rgba8Rmse(front, repeat));
            Renderer3D::GetPostProcessSettings().TAAEnabled = true;
            for (u32 frame = 0; frame < 8; ++frame)
            {
                Pose({ -8.0f + static_cast<f32>(frame), 4.0f, 14.0f });
                GetScene().GetStreamingSettings().RepresentationResidentMegabytes = frame < 4 ? 0.001f : 64.0f;
                RunFrames(1);
                Capture(stem + "_Motion" + std::to_string(frame));
            }
            Renderer3D::GetPostProcessSettings().TAAEnabled = false;
            GetScene().GetStreamingSettings().RepresentationResidentMegabytes = 64.0f;
        }
    }
} // namespace OloEngine::Tests
