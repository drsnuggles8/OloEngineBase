// OLO_TEST_LAYER: plumbing
// =============================================================================
// RendererMemoryAccountingEvidenceTest.cpp — issue #1342, the OpenGL half.
//
// RendererMemoryReportTest.cpp pins the report's rules on fake addresses. This file
// renders real frames through the real pipeline and checks that the report the editor,
// olo_memory_report and the benchmark export read tells the truth about them:
//
//   * every path x MSAA x render-scale cell publishes capacity-versus-demand rows, and
//     each owner's capacity row agrees BYTE FOR BYTE with the physical bytes the table
//     attributes to that owner (two independent routes to one number);
//   * a real view of a real texture (the shadow compare-off view) is an alias, and the
//     #1342 negative control makes the snapshot check fail on it;
//   * resize, a texture reload and a temporal-history format change each show their
//     old+new coexistence as retiring bytes and a window peak, and reclaim them once the
//     frames that could still read the old storage complete — and not before (the
//     delayed-fence case);
//   * repeated scene load/unload returns to the same resident bytes.
//
// Each cell writes its report to OloEditor/assets/tests/memory/ — the artefact the PR's
// verification matrix cites. Skips cleanly without a GL 4.6 context (never DISABLED_).
//
// OpenGL reports residency as UNKNOWN and reconciliation as NOT OBSERVABLE: it has no
// allocator to ask. The Vulkan half — VMA reconciliation, OS residency, the fault turning
// the reconciliation to overCounted — is VulkanPassSuite's memory tenant.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReportJson.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <fstream>
#include <string>
#include <string_view>

using namespace OloEngine;        // NOLINT(google-build-using-namespace)
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // Frames enough for FrameResourceManager (two buffered frames) to drain every deferred
    // deletion queued before the call.
    constexpr u32 kDrainFrames = 4;

    [[nodiscard]] u64 OwnerLiveGpuBytes(const RendererMemoryReport& report, std::string_view owner)
    {
        u64 bytes = 0;
        for (const auto& row : report.Owners)
            if (row.Owner.ToView() == owner)
                bytes += row.GpuLiveBytes;
        return bytes;
    }

    [[nodiscard]] const MemoryCapacityRow* FindCapacity(const RendererMemoryReport& report, std::string_view owner,
                                                        std::string_view categoryPrefix)
    {
        for (const auto& row : report.Capacity)
            if (row.Owner.ToView() == owner && row.Category.ToView().starts_with(categoryPrefix))
                return &row;
        return nullptr;
    }

    [[nodiscard]] u64 CapacityTotal(const RendererMemoryReport& report, std::string_view owner)
    {
        u64 bytes = 0;
        for (const auto& row : report.Capacity)
            if (row.Owner.ToView() == owner && row.IsGpu)
                bytes += row.CapacityBytes.value_or(0);
        return bytes;
    }

    void WriteArtefact(const std::string& cellName, const RendererMemoryReport& report)
    {
        const fs::path dir = fs::path("assets") / "tests" / "memory";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out(dir / ("RendererMemory_" + cellName + ".json"), std::ios::binary | std::ios::trunc);
        out << RendererMemoryReportToJson(report).dump(2) << '\n';
    }

    // The two passes that keep their own scene framebuffer (#1546) need content the cubes
    // below never give them: a lawn for FoliageRenderPass and a groom for GroomRenderPass.
    [[nodiscard]] Entity AddLawn(Scene& scene)
    {
        Entity lawn = scene.CreateEntity("Lawn");
        lawn.GetComponent<TransformComponent>().Translation = { -8.0f, -0.6f, -8.0f }; // [0, size] local
        auto& terrain = lawn.AddComponent<TerrainComponent>();
        terrain.m_ProceduralEnabled = true;
        terrain.m_ProceduralSeed = 1546;
        terrain.m_ProceduralResolution = 32;
        terrain.m_ProceduralOctaves = 1;
        terrain.m_WorldSizeX = 16.0f;
        terrain.m_WorldSizeZ = 16.0f;
        terrain.m_HeightScale = 0.1f;
        terrain.m_TessellationEnabled = false;
        terrain.m_Material = Ref<TerrainMaterial>::Create();
        for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
            terrain.m_Material->AddLayer(layer);

        auto& foliage = lawn.AddComponent<FoliageComponent>();
        foliage.m_Enabled = true;
        FoliageLayer grass;
        grass.Name = "Grass";
        grass.AlbedoPath = "assets/textures/grass.png";
        grass.UseAuthoredMesh = false; // cards: the foliage pass, without a mesh to load
        grass.Density = 2.0f;
        grass.SplatmapChannel = -1;
        grass.MinHeight = 0.2f;
        grass.MaxHeight = 0.4f;
        grass.FadeStartDistance = 40.0f;
        grass.ViewDistance = 50.0f;
        foliage.m_Layers.Add(grass);
        foliage.m_NeedsRebuild = true;
        return lawn;
    }

    [[nodiscard]] Ref<GroomAsset> BuildTuft()
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("tuft", group, reason)) << reason;
        const std::vector<f32> widths = { 0.004f, 0.004f, 0.003f, 0.002f };
        for (u32 s = 0; s < 400u; ++s)
        {
            const f32 x = (static_cast<f32>(s % 20u) - 9.5f) * 0.02f;
            const f32 z = (static_cast<f32>(s / 20u) - 9.5f) * 0.02f;
            const std::vector<glm::vec3> points = { { x, 0.0f, z }, { x, 0.1f, z }, { x, 0.2f, z }, { x, 0.3f, z } };
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { 0.5f, 0.5f };
            input.GroupId = group;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        builder.SetName("MemoryAccountingTuft");
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        if (groom)
            EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
        return groom;
    }

    // A groom is resolved through the asset manager, which a renderer fixture alone has not.
    void EnsureAssetManager()
    {
        if (Project::GetActive() && Project::HasAssetManager())
            return;
        std::error_code ec;
        const fs::path projectDir = TempDir("memory-accounting-project");
        fs::create_directories(projectDir / "Assets", ec);
        ASSERT_FALSE(ec) << ec.message();
        std::ofstream(projectDir / "Evidence.oloproj") << "Project:\n"
                                                          "  Name: MemoryAccountingEvidence\n"
                                                          "  StartScene: \"\"\n"
                                                          "  AssetDirectory: \"Assets\"\n"
                                                          "  ScriptModulePath: \"\"\n";
        ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize(false);
        Project::SetAssetManager(assetManager);
    }

    class ScopedFaultCountAliasAsBacking
    {
      public:
        ScopedFaultCountAliasAsBacking() : m_Previous(Levers::FaultCountAliasAsBacking())
        {
            Levers::SetFaultCountAliasAsBacking(true);
        }
        ~ScopedFaultCountAliasAsBacking()
        {
            Levers::SetFaultCountAliasAsBacking(m_Previous);
        }
        ScopedFaultCountAliasAsBacking(const ScopedFaultCountAliasAsBacking&) = delete;
        ScopedFaultCountAliasAsBacking& operator=(const ScopedFaultCountAliasAsBacking&) = delete;

      private:
        bool m_Previous;
    };

    class RendererMemoryAccountingEvidence : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(256, 256);
            PopulateScene(GetScene());
        }

        static void PopulateScene(Scene& scene)
        {
            Entity cameraEntity = scene.CreateEntity("PrimaryCamera");
            auto& camera = cameraEntity.AddComponent<CameraComponent>();
            camera.Primary = true;
            cameraEntity.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 6.0f };

            // A shadow-casting sun, so the CSM is demanded and not just allocated.
            Entity sun = scene.CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.3f, -0.9f, 0.2f));
            light.m_Intensity = 3.0f;

            for (i32 i = 0; i < 3; ++i)
            {
                Entity cube = scene.CreateEntity("Cube" + std::to_string(i));
                cube.AddComponent<MeshComponent>();
                cube.GetComponent<TransformComponent>().Translation = { static_cast<f32>(i) * 1.5f - 1.5f, 0.0f, 0.0f };
            }
        }

        [[nodiscard]] static RendererMemoryReport Report()
        {
            return RendererMemoryTracker::GetInstance().BuildReport();
        }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// Criterion 3 + 4: every cell of path x MSAA x render scale publishes capacity versus
// demand for the owners the issue names, and the rows agree with the physical table.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, EveryPathMsaaAndScaleCellPublishesCapacityVersusDemand)
{
    OLO_ENSURE_GPU_OR_SKIP();

    struct Cell
    {
        const char* Name;
        RenderingPath Path;
        u32 Msaa;
        UpscaleMode Upscale;
    };
    static constexpr std::array<Cell, 5> kCells = { {
        { "GL_Forward_Native", RenderingPath::Forward, 1u, UpscaleMode::Off },
        { "GL_ForwardPlus_Native", RenderingPath::ForwardPlus, 1u, UpscaleMode::Off },
        { "GL_Deferred_Native", RenderingPath::Deferred, 1u, UpscaleMode::Off },
        { "GL_Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off },
        { "GL_Deferred_UpscalePerformance", RenderingPath::Deferred, 1u, UpscaleMode::Performance },
    } };

    for (const Cell& cell : kCells)
    {
        SCOPED_TRACE(cell.Name);
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        settings.Path = cell.Path;
        settings.Deferred.MSAASampleCount = cell.Msaa;
        Renderer3D::GetPostProcessSettings().Upscale = cell.Upscale;
        Renderer3D::ApplyRendererSettings();
        if (Renderer3D::GetRendererSettings().Deferred.MSAASampleCount != cell.Msaa)
        {
            std::cout << "[ memory ] " << cell.Name << ": device returned "
                      << Renderer3D::GetRendererSettings().Deferred.MSAASampleCount << " samples; cell not run\n";
            continue;
        }
        RunFrames(kDrainFrames + 2);

        const RendererMemoryReport report = Report();
        WriteArtefact(cell.Name, report);

        // Backend validity: GL can observe neither an allocator total nor residency.
        EXPECT_EQ(report.Reconciliation.Status, MemoryReconciliationStatus::NotObservable);
        EXPECT_EQ(report.Observation.Residency, MemoryResidencyStatus::Unknown);
        EXPECT_GT(report.Gpu.LiveBytes, 0u);
        EXPECT_EQ(report.Gpu.CommittedBytes, 0u) << "GL has no committed sizes; every GPU byte is a format estimate";

        // Transient pool: capacity covers demand, and agrees with the physical table.
        const MemoryCapacityRow* pool = FindCapacity(report, "TransientPool", "Render-graph transients");
        ASSERT_NE(pool, nullptr);
        ASSERT_TRUE(pool->CapacityBytes && pool->ActiveDemandBytes);
        EXPECT_GT(*pool->ActiveDemandBytes, 0u);
        EXPECT_GE(*pool->CapacityBytes, *pool->ActiveDemandBytes);
        // Every live RenderGraph has its own pool and row; the physical table has one owner.
        EXPECT_EQ(CapacityTotal(report, "TransientPool"), OwnerLiveGpuBytes(report, "TransientPool"))
            << "the pools' capacity rows and the bytes their own allocations booked disagree";

        // G-buffer: demanded exactly when the Deferred path renders through it.
        const MemoryCapacityRow* gbuffer = FindCapacity(report, "SceneRenderPass", "G-buffer");
        ASSERT_NE(gbuffer, nullptr);
        ASSERT_TRUE(gbuffer->CapacityBytes && gbuffer->ActiveDemandBytes);
        if (cell.Path == RenderingPath::Deferred)
        {
            EXPECT_GT(*gbuffer->ActiveDemandBytes, 0u);
            EXPECT_EQ(CapacityTotal(report, "SceneRenderPass"), OwnerLiveGpuBytes(report, "GBuffer"))
                << "the G-buffer rows and the G-buffer's own allocations disagree";
        }
        else
        {
            EXPECT_EQ(*gbuffer->ActiveDemandBytes, 0u) << "the G-buffer was demanded off the Deferred path";
        }

        // Skin hand-off: a view inside SceneColor, never backing of its own.
        const MemoryCapacityRow* skin = FindCapacity(report, "RenderGraph", "Skin hand-off");
        ASSERT_NE(skin, nullptr);
        EXPECT_EQ(skin->CapacityBytes, skin->AliasSavingsBytes);

        // Shadows: the sun is casting, so the CSM is demanded.
        const MemoryCapacityRow* csm = FindCapacity(report, "ShadowMap", "Directional CSM");
        ASSERT_NE(csm, nullptr);
        ASSERT_TRUE(csm->CapacityBytes && csm->ActiveDemandBytes);
        EXPECT_GT(*csm->ActiveDemandBytes, 0u);
        const MemoryCapacityRow* atlas = FindCapacity(report, "ShadowMap", "Local-light shadow atlas");
        ASSERT_NE(atlas, nullptr);
        EXPECT_LE(csm->CapacityBytes.value_or(0) + atlas->CapacityBytes.value_or(0), OwnerLiveGpuBytes(report, "ShadowMap"))
            << "the CSM and atlas rows claim more than the shadow map's own allocations booked";

        // Temporal histories agree with the table too (whatever this cell declares).
        EXPECT_EQ(CapacityTotal(report, "TemporalHistory"), OwnerLiveGpuBytes(report, "TemporalHistory"));

        // Ray tracing on GL: the rows exist and say why they are empty.
        const MemoryCapacityRow* as = FindCapacity(report, "RayTracingScene", "Acceleration structures");
        ASSERT_NE(as, nullptr);
        EXPECT_EQ(as->CapacityBytes.value_or(1), 0u);
        EXPECT_FALSE(as->UnknownReason.IsEmpty());

        std::cout << "[ memory ] " << cell.Name << ": gpu live " << report.Gpu.LiveBytes << " retiring " << report.Gpu.RetiringBytes
                  << ", pool capacity " << *pool->CapacityBytes << " demand " << *pool->ActiveDemandBytes << " alias savings "
                  << pool->AliasSavingsBytes.value_or(0) << ", gbuffer " << *gbuffer->CapacityBytes << "/"
                  << *gbuffer->ActiveDemandBytes << "\n";
    }

    Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
    Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
    Renderer3D::ApplyRendererSettings();
}

// ---------------------------------------------------------------------------------------
// Criterion 1 + 4, on real GPU objects: a view is an alias, and the negative control
// turns the snapshot check red.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, ACompareOffViewIsAnAliasAndTheFaultMakesItADoubleCount)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Texture2DArraySpecification spec;
    spec.Width = 64;
    spec.Height = 64;
    spec.Layers = 4;
    spec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
    spec.DepthComparisonMode = true;
    constexpr u64 kBackingBytes = 64ull * 64ull * 4ull * 4ull;

    const auto viewCycle = [&spec](const bool fault) -> std::pair<i64, u32>
    {
        std::optional<ScopedFaultCountAliasAsBacking> faultScope;
        if (fault)
            faultScope.emplace();

        Ref<Texture2DArray> backing = Texture2DArray::Create(spec);
        const RendererMemoryReport withBacking = RendererMemoryTracker::GetInstance().BuildReport();
        const RHI::ResourceHandle view = RenderCommand::CreateDepthArrayCompareOffViewHandle(backing->GetRHIHandle(), spec.Layers);
        const RendererMemoryReport withView = RendererMemoryTracker::GetInstance().BuildReport();
        RenderCommand::DeleteTexture(view);
        const RendererMemoryReport afterDelete = RendererMemoryTracker::GetInstance().BuildReport();
        EXPECT_EQ(afterDelete.AliasCount, withBacking.AliasCount) << "deleting the view left its alias behind";
        backing = nullptr;
        return { static_cast<i64>(withView.Gpu.ResidentBytes()) - static_cast<i64>(withBacking.Gpu.ResidentBytes()),
                 withView.AliasCount - withBacking.AliasCount };
    };

    const RendererMemoryReport before = RendererMemoryTracker::GetInstance().BuildReport();
    Ref<Texture2DArray> probe = Texture2DArray::Create(spec);
    EXPECT_EQ(RendererMemoryTracker::GetInstance().BuildReport().Gpu.ResidentBytes() - before.Gpu.ResidentBytes(), kBackingBytes)
        << "the backing texture did not book its storage";
    probe = nullptr;

    const auto [cleanDelta, cleanAliases] = viewCycle(false);
    EXPECT_EQ(cleanDelta, 0) << "creating a view added physical bytes: a view was counted as backing";
    EXPECT_EQ(cleanAliases, 1u) << "the view was not booked as an alias";

    // Negative control: the same sequence with views booked as backing. The check above
    // must see the double count; if it does not, the check is vacuous.
    const auto [faultDelta, faultAliases] = viewCycle(true);
    EXPECT_EQ(faultDelta, static_cast<i64>(kBackingBytes)) << "the planted double count did not reach the physical totals";
    EXPECT_EQ(faultAliases, 0u);

    RunFrames(kDrainFrames);
}

// ---------------------------------------------------------------------------------------
// Criterion 2: resize — peak coexistence, delayed-fence retention, reclamation.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, ResizeShowsOldAndNewTogetherThenReclaimsTheOld)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto& tracker = RendererMemoryTracker::GetInstance();
    Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
    Renderer3D::ApplyRendererSettings();
    RunFrames(kDrainFrames + 2);
    const RendererMemoryReport steady = Report();

    tracker.BeginPeakWindow();
    ResizeRenderTarget(512, 384);
    // No frame has completed since the resize: every replaced target is still resident.
    const RendererMemoryReport rightAfter = Report();
    EXPECT_GT(rightAfter.Gpu.RetiringBytes, steady.Gpu.RetiringBytes)
        << "the resize replaced its targets but nothing is retiring: the old storage was dropped from the books "
           "while the GPU could still read it";

    RunFrames(1);
    const RendererMemoryReport oneFrame = Report();
    RunFrames(kDrainFrames);
    const RendererMemoryReport drained = Report();

    EXPECT_EQ(drained.Gpu.RetiringBytes, steady.Gpu.RetiringBytes) << "retired targets were never reclaimed";
    EXPECT_GE(drained.Gpu.WindowPeakBytes, rightAfter.Gpu.ResidentBytes())
        << "the window peak lost the old+new coexistence";
    EXPECT_GT(drained.Gpu.WindowPeakBytes, drained.Gpu.ResidentBytes())
        << "the transient peak should exceed the settled footprint: old and new coexisted";

    std::cout << "[ memory ] resize 256x256 -> 512x384: steady " << steady.Gpu.ResidentBytes() << ", right after "
              << rightAfter.Gpu.ResidentBytes() << " (retiring " << rightAfter.Gpu.RetiringBytes << "), +1 frame "
              << oneFrame.Gpu.ResidentBytes() << " (retiring " << oneFrame.Gpu.RetiringBytes << "), drained "
              << drained.Gpu.ResidentBytes() << ", window peak " << drained.Gpu.WindowPeakBytes << "\n";

    // Back to the original size: the settled footprint returns to where it started. Pool
    // buckets for the other size that nobody demands must not survive as capacity.
    ResizeRenderTarget(256, 256);
    RunFrames(kDrainFrames + 2);
    const RendererMemoryReport back = Report();
    std::cout << "[ memory ] resize back to 256x256: resident " << back.Gpu.ResidentBytes() << " (steady was "
              << steady.Gpu.ResidentBytes() << ")\n";
    EXPECT_EQ(back.Gpu.ResidentBytes(), steady.Gpu.ResidentBytes())
        << "a resize round trip left " << static_cast<i64>(back.Gpu.ResidentBytes()) - static_cast<i64>(steady.Gpu.ResidentBytes())
        << " bytes resident";
}

// ---------------------------------------------------------------------------------------
// Criterion 2: a hot reload replaces storage in place; the old storage retires.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, ATextureReloadRetiresTheOldStorageUntilItsFramesComplete)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TextureSpecification spec;
    spec.Width = 128;
    spec.Height = 128;
    spec.Format = ImageFormat::RGBA16F;
    spec.GenerateMips = true;
    Ref<Texture2D> texture = Texture2D::Create(spec);
    RunFrames(kDrainFrames);

    const RendererMemoryReport before = Report();
    // A same-format storage recreate: the path Reload() and Resize() share.
    texture->Resize(256, 256);
    const RendererMemoryReport during = Report();
    const u64 oldBytes = RendererMemoryFormat::ImageBytes(ImageFormat::RGBA16F, 128, 128, RendererMemoryFormat::FullMipCount(128, 128)).value();
    const u64 newBytes = RendererMemoryFormat::ImageBytes(ImageFormat::RGBA16F, 256, 256, RendererMemoryFormat::FullMipCount(256, 256)).value();
    EXPECT_EQ(during.Gpu.RetiringBytes - before.Gpu.RetiringBytes, oldBytes) << "the old storage is not retiring";
    EXPECT_EQ(during.Gpu.LiveBytes - before.Gpu.LiveBytes + oldBytes, newBytes) << "the new storage is not live at its full size";

    RunFrames(kDrainFrames);
    EXPECT_EQ(Report().Gpu.RetiringBytes, before.Gpu.RetiringBytes) << "the reloaded texture's old storage was never reclaimed";
    texture = nullptr;
    RunFrames(kDrainFrames);
}

// ---------------------------------------------------------------------------------------
// Criterion 2: a temporal history whose format changes coexists with its replacement.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, AHistoryFormatChangeCoexistsThenReclaims)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<RenderGraph> graph = Ref<RenderGraph>::Create();
    graph->Init(96, 64);
    const TemporalHistoryKey key{ .Effect = TemporalHistoryEffect::TAA, .View = 1342u };
    TemporalHistoryDescriptor descriptor;
    descriptor.Width = 96;
    descriptor.Height = 64;
    descriptor.Format = ImageFormat::RGBA16F;

    (void)graph->AcquireTemporalHistory(key, descriptor, TemporalHistoryDependency::None, "MemoryTestHistory");
    RunFrames(kDrainFrames);
    const RendererMemoryReport before = Report();

    descriptor.Format = ImageFormat::RGBA32F;
    (void)graph->AcquireTemporalHistory(key, descriptor, TemporalHistoryDependency::None, "MemoryTestHistory");
    const RendererMemoryReport during = Report();
    EXPECT_EQ(during.Gpu.RetiringBytes - before.Gpu.RetiringBytes, 96u * 64u * 8u) << "the RGBA16F history is not retiring";
    EXPECT_EQ(during.Gpu.LiveBytes - before.Gpu.LiveBytes, 96u * 64u * (16u - 8u)) << "the RGBA32F replacement is not live";

    RunFrames(kDrainFrames);
    EXPECT_EQ(Report().Gpu.RetiringBytes, before.Gpu.RetiringBytes) << "the old history was never reclaimed";

    graph->Shutdown();
    graph = nullptr;
    RunFrames(kDrainFrames);
}

// ---------------------------------------------------------------------------------------
// Criterion 2 (amendment): repeated scene load/unload returns to the same footprint.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, RepeatedSceneLoadAndUnloadReturnsToTheSameFootprint)
{
    OLO_ENSURE_GPU_OR_SKIP();

    EditorCamera camera(55.0f, 1.0f, 0.05f, 400.0f);
    camera.SetViewportSize(256.0f, 256.0f);
    RunFrames(kDrainFrames);

    std::array<u64, 3> settled{};
    for (u32 cycle = 0; cycle < settled.size(); ++cycle)
    {
        {
            Ref<Scene> other = Scene::Create();
            other->SetIs3DModeEnabled(true);
            other->OnViewportResize(256, 256);
            other->SetRenderingEnabled(true);
            PopulateScene(*other);
            RunEditorFramesOn(*other, camera, 3);
        }
        RunFrames(kDrainFrames + 2);
        settled[cycle] = Report().Gpu.ResidentBytes();
        std::cout << "[ memory ] scene cycle " << cycle << ": resident " << settled[cycle] << "\n";
    }
    // The first cycle may warm caches the renderer keeps; every later one must not grow.
    EXPECT_EQ(settled[2], settled[1]) << "each scene load/unload cycle left bytes behind";
}

// ---------------------------------------------------------------------------------------
// Found and fixed (#1342): an environment map booked an aggregate "rough estimate" of its
// textures on top of the cubemaps that book themselves — one extra TextureCubemap booking
// per environment map, a double count of its storage. Building one from an existing sky
// must add exactly the cubemaps it creates (irradiance and prefilter), and no more.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, AnEnvironmentMapBooksOnlyTheCubemapsItCreates)
{
    OLO_ENSURE_GPU_OR_SKIP();

    CubemapSpecification skySpec;
    skySpec.Width = 32;
    skySpec.Height = 32;
    skySpec.Format = ImageFormat::RGBA32F; // the format GL cubemaps and CreateFromCubemap use
    Ref<TextureCubemap> sky = TextureCubemap::Create(skySpec);
    ASSERT_TRUE(sky);

    auto& tracker = RendererMemoryTracker::GetInstance();
    const u32 cubemapsBefore = tracker.GetAllocationCount(RendererMemoryTracker::ResourceType::TextureCubemap);
    Ref<EnvironmentMap> environment = EnvironmentMap::CreateFromCubemap(sky);
    ASSERT_TRUE(environment);

    u32 created = 0;
    for (const Ref<TextureCubemap>* map : { &environment->GetIrradianceMap(), &environment->GetPrefilterMap() })
    {
        if (*map && map->Raw() != sky.Raw())
            ++created;
    }
    EXPECT_GT(created, 0u) << "the environment map generated no IBL cubemaps; nothing was measured";
    EXPECT_EQ(tracker.GetAllocationCount(RendererMemoryTracker::ResourceType::TextureCubemap) - cubemapsBefore, created)
        << "an environment map booked more cubemaps than it holds: its storage is counted twice";

    environment = nullptr;
    sky = nullptr;
    RunFrames(kDrainFrames);
}

// ---------------------------------------------------------------------------------------
// Found and fixed (#1342): a path switch left the old path's scene framebuffer resident.
// Two holders, found by this report's capacity-versus-physical cross-check: a pass that
// stops re-resolving its target (GPUDrivenOcclusionPass on Deferred) and a pass the new
// topology no longer registers (DeferredLightingPass on Forward) each kept a Ref to a pooled
// framebuffer after the pool had evicted it — a full-resolution MRT, ~300 MB at 4K. After a
// switch in EITHER direction the pools must hold every pool-created object still alive.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, APathSwitchLeavesNoPooledFramebufferBehind)
{
    OLO_ENSURE_GPU_OR_SKIP();

    for (const RenderingPath path : { RenderingPath::Deferred, RenderingPath::Forward, RenderingPath::Deferred,
                                      RenderingPath::ForwardPlus })
    {
        Renderer3D::GetRendererSettings().Path = path;
        Renderer3D::ApplyRendererSettings();
        RunFrames(kDrainFrames + 2);
        const RendererMemoryReport report = Report();
        EXPECT_EQ(CapacityTotal(report, "TransientPool"), OwnerLiveGpuBytes(report, "TransientPool"))
            << "after switching to path " << static_cast<int>(path) << ", "
            << static_cast<i64>(OwnerLiveGpuBytes(report, "TransientPool")) - static_cast<i64>(CapacityTotal(report, "TransientPool"))
            << " bytes of pool-created framebuffers are alive outside every pool";
    }
}

// ---------------------------------------------------------------------------------------
// #1546: GroomRenderPass and FoliageRenderPass keep the scene framebuffer they draw into in
// a member of their own, which the scene above never reaches. Off the Forward path the
// foliage pass stops re-resolving it, and a groom pass with nothing to draw is culled; each
// then pinned a pooled framebuffer the pool had evicted, a full-resolution MRT for as long
// as the pass stayed idle.
// ---------------------------------------------------------------------------------------
TEST_F(RendererMemoryAccountingEvidence, AGroomAndALawnLeaveNoPooledFramebufferBehind)
{
    OLO_ENSURE_GPU_OR_SKIP();
    EnsureAssetManager();
    ASSERT_FALSE(HasFatalFailure());

    Entity lawn = AddLawn(GetScene());
    const Ref<GroomAsset> tuft = BuildTuft();
    ASSERT_TRUE(tuft);
    Entity groom = GetScene().CreateEntity("Tuft");
    groom.GetComponent<TransformComponent>().Translation = { 0.0f, -0.4f, 3.0f };
    auto& groomComponent = groom.AddComponent<GroomComponent>();
    groomComponent.m_Groom = AssetManager::AddMemoryOnlyAsset<GroomAsset>(tuft);
    groomComponent.m_ShowPreview = false;
    groomComponent.m_RenderStrands = true;

    const auto usePath = [this](const RenderingPath path)
    {
        Renderer3D::GetRendererSettings().Path = path;
        Renderer3D::ApplyRendererSettings();
        RunFrames(kDrainFrames + 2);
    };
    const auto expectPoolsHoldEverything = [](const char* step)
    {
        const RendererMemoryReport report = Report();
        EXPECT_EQ(CapacityTotal(report, "TransientPool"), OwnerLiveGpuBytes(report, "TransientPool"))
            << step << ": "
            << static_cast<i64>(OwnerLiveGpuBytes(report, "TransientPool")) - static_cast<i64>(CapacityTotal(report, "TransientPool"))
            << " bytes of pool-created framebuffers are alive outside every pool";
    };

    usePath(RenderingPath::Forward);
    const GroomRenderPass* groomPass = Renderer3D::GetGroomRenderPass();
    ASSERT_NE(groomPass, nullptr);
    ASSERT_GT(groomPass->GetStats().StrandsDrawn, 0u) << "the groom pass never drew, so this says nothing about it";
    const auto& foliage = lawn.GetComponent<FoliageComponent>();
    ASSERT_TRUE(foliage.m_Renderer && foliage.m_Renderer->GetActiveLayerDrawInfo().Num() > 0)
        << "the lawn never drew, so this says nothing about the foliage pass";
    expectPoolsHoldEverything("Forward");

    // Deferred draws the lawn into the G-Buffer: the foliage pass's Forward target idles.
    usePath(RenderingPath::Deferred);
    expectPoolsHoldEverything("Deferred, after Forward");

    // With the groom gone its pass is culled, holding the last target it drew into.
    GetScene().DestroyEntity(groom);
    usePath(RenderingPath::Forward);
    expectPoolsHoldEverything("Forward, with the groom gone");
    usePath(RenderingPath::Deferred);
    expectPoolsHoldEverything("Deferred, with the groom gone");
}
