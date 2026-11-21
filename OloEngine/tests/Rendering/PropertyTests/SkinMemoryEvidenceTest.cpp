// OLO_TEST_LAYER: plumbing
// =============================================================================
// SkinMemoryEvidenceTest.cpp — the skin stack's rows in the renderer memory
// report. Issue #1396.
//
// #1222's contract asks for "memory on named hardware" for the skin stack, and
// until #1396 the report had one skin row (the SceneColor hand-off view). The
// diffusion's scratch target, the two snapshots it samples, its kernel UBO and
// the deferred profile table were inside other owners' totals with nothing
// saying how much of them skin was responsible for.
//
// Each row is checked against a PREDICTION, not just "non-zero": the scratch is
// one RGBA16F image at the scene band, the snapshots are one RGBA16F plus one
// D24S8 image at the scene size, so a row that counted the wrong resource (or
// one of them twice) fails by a byte count. And a scene with no skin must
// report zero on every skin row: a skinless scene that still pays for the
// diffusion is the failure this pins.
//
// Runs in the GL process and in `OloEngine-Tests --olo-rhi=vulkan`; the report
// JSON for each cell lands in OloEditor/assets/tests/memory/ for the PR's
// verification matrix. Skips cleanly without a GPU (never DISABLED_).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReportJson.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Not square, so a width/height swap in any row is a byte-count failure.
        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 192;
        constexpr u64 kPixels = static_cast<u64>(kWidth) * kHeight;
        constexpr u64 kRgba16fBytes = 8;
        constexpr u64 kD24S8Bytes = 4;

        [[nodiscard]] const MemoryCapacityRow* FindRow(const RendererMemoryReport& report, std::string_view owner,
                                                       std::string_view categoryPrefix)
        {
            for (const auto& row : report.Capacity)
                if (row.Owner.ToView() == owner && row.Category.ToView().starts_with(categoryPrefix))
                    return &row;
            return nullptr;
        }

        struct SkinRows
        {
            u64 Scratch = 0;
            u64 Snapshots = 0;
            u64 UboCapacity = 0;
            u64 UboDemand = 0;
            u64 TableCapacity = 0;
            u64 TableDemand = 0;
            u64 PoolDemand = 0;
        };

        // Every row must exist and carry a figure; a missing row or an empty optional
        // is a failure, never a 0.
        [[nodiscard]] SkinRows ReadSkinRows(const RendererMemoryReport& report)
        {
            SkinRows out;
            const auto read = [&report](std::string_view owner, std::string_view category, u64& capacity, u64* demand)
            {
                const MemoryCapacityRow* row = FindRow(report, owner, category);
                EXPECT_NE(row, nullptr) << "no " << owner << " / " << category << " row";
                if (!row)
                    return;
                EXPECT_TRUE(row->CapacityBytes.has_value() && row->ActiveDemandBytes.has_value())
                    << owner << " / " << category << ": " << row->UnknownReason.ToView();
                capacity = row->CapacityBytes.value_or(0);
                if (demand)
                    *demand = row->ActiveDemandBytes.value_or(0);
            };
            read("SkinDiffusion", "Diffusion scratch target", out.Scratch, nullptr);
            read("SkinDiffusion", "Diffusion input snapshots", out.Snapshots, nullptr);
            read("SkinDiffusion", "Kernel UBO", out.UboCapacity, &out.UboDemand);
            read("SkinProfileTable", "Deferred profile table", out.TableCapacity, &out.TableDemand);
            u64 poolCapacity = 0;
            read("TransientPool", "Render-graph transients", poolCapacity, &out.PoolDemand);
            return out;
        }

        void WriteArtefact(const std::string& cellName, const RendererMemoryReport& report)
        {
            const fs::path dir = fs::path("assets") / "tests" / "memory";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::ofstream out(dir / (cellName + ".json"), std::ios::binary | std::ios::trunc);
            out << RendererMemoryReportToJson(report).dump(2) << '\n';
        }

        struct PathCell
        {
            RenderingPath Path;
            const char* Name;
        };
        constexpr std::array<PathCell, 3> kPaths = { {
            { RenderingPath::Forward, "Forward" },
            { RenderingPath::ForwardPlus, "ForwardPlus" },
            { RenderingPath::Deferred, "Deferred" },
        } };
    } // namespace

    class SkinMemoryEvidence : public RendererAttachedTest
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            m_Previous = Project::GetActive();
            m_Manager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_Saved = true;
            ProjectConfig config;
            config.AssetDirectory = "Assets";
            ASSERT_TRUE(Project::NewInMemory(TempDir(), config));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);

            auto profile = Ref<SkinProfile>::Create();
            profile->SetName("SkinMemoryProbe");
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;
            ASSERT_TRUE(profile->SetParameters(parameters));
            m_Profile = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            ASSERT_NE(static_cast<u64>(m_Profile), 0ULL);

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;

            Entity sun = GetScene().CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.6f, -0.4f, -0.7f));
            light.m_Intensity = 2.0f;
            light.m_CastShadows = false;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 32);
            m_Sphere = GetScene().CreateEntity("Head");
            m_Sphere.AddComponent<MeshComponent>(sphere->GetMeshSource());
            auto& material = m_Sphere.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor(glm::vec4(0.62f, 0.48f, 0.42f, 1.0f));
            material.SetRoughnessFactor(0.4f);

            Renderer3D::GetRendererSettings().ForwardPlusAutoSwitch = false;
            EnableRendering(kWidth, kHeight);
        }

        void TearDown() override
        {
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
            Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
            Renderer3D::GetSkinProfileTable().Reset();
            RendererAttachedTest::TearDown();
            if (m_Saved)
                Project::RestoreActive(m_Previous, m_Manager);
        }

        void SetSkin(bool skin)
        {
            auto& material = m_Sphere.GetComponent<MaterialComponent>().m_Material;
            material.SetMaterialKind(skin ? MaterialKind::Skin : MaterialKind::Generic);
            material.SetSkinProfileHandle(skin ? m_Profile : AssetHandle{});
            // Slots are sticky until a reset (a scene load); this is that reset.
            Renderer3D::GetSkinProfileTable().Reset();
        }

        [[nodiscard]] RendererMemoryReport RenderCell(RenderingPath path, std::string_view feature, std::string_view cell)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
            // A path or content change rebuilds the graph; the frames after it are cached
            // ones, which is the state the rows must survive.
            RunFrames(4);
            RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();
            WriteArtefact(TestRenderBackend::EvidenceName(feature) + std::string(cell), report);
            return report;
        }

        AssetHandle m_Profile{};
        Entity m_Sphere{};
        Ref<Project> m_Previous;
        Ref<AssetManagerBase> m_Manager;
        bool m_Saved = false;
    };

    TEST_F(SkinMemoryEvidence, ASkinSceneReportsTheDiffusionBytesItPredictsOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetSkin(true);
        for (const PathCell& cell : kPaths)
        {
            SCOPED_TRACE(cell.Name);
            const SkinRows rows = ReadSkinRows(RenderCell(cell.Path, "SkinMemory", cell.Name));

            EXPECT_EQ(rows.Scratch, kPixels * kRgba16fBytes) << "the scratch is one RGBA16F image at the scene band";
            EXPECT_EQ(rows.Snapshots, kPixels * (kRgba16fBytes + kD24S8Bytes))
                << "the snapshots are one RGBA16F hand-off and one D24S8 depth at the scene size";
            EXPECT_EQ(rows.UboCapacity, static_cast<u64>(SkinDiffusionUBOData::GetSize()));
            EXPECT_EQ(rows.UboDemand, rows.UboCapacity) << "the diffusion did not draw with a diffusing profile on screen";
            EXPECT_GE(rows.PoolDemand, rows.Scratch + rows.Snapshots) << "skin claims more than the pool demanded";

            // 15 slots x (specular tint, two transmission lanes, lobe, oral lane).
            EXPECT_EQ(rows.TableCapacity, static_cast<u64>(kMaxSkinProfileSlots) * 5u * 16u);
            EXPECT_EQ(rows.TableDemand, cell.Path == RenderingPath::Deferred ? rows.TableCapacity : 0u)
                << "the deferred table is read only by the Deferred lighting pass";

            std::cout << "[ skin memory ] " << TestRenderBackend::EvidenceName("SkinMemory") << cell.Name << ": scratch " << rows.Scratch
                      << " snapshots " << rows.Snapshots << " ubo " << rows.UboCapacity << " table " << rows.TableCapacity
                      << " pool demand " << rows.PoolDemand << "\n";
        }
    }

    TEST_F(SkinMemoryEvidence, ASceneWithoutSkinReportsZeroOnEverySkinRow)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetSkin(false);
        for (const PathCell& cell : kPaths)
        {
            SCOPED_TRACE(cell.Name);
            const SkinRows rows = ReadSkinRows(RenderCell(cell.Path, "SkinMemoryOff", cell.Name));
            // Non-vacuity: the frame rendered and the pool demanded something.
            EXPECT_GT(rows.PoolDemand, 0u);
            EXPECT_EQ(rows.Scratch, 0u) << "a scene with no skin allocated the diffusion scratch";
            EXPECT_EQ(rows.Snapshots, 0u) << "a scene with no skin (and no snow) allocated the diffusion snapshots";
            EXPECT_EQ(rows.UboDemand, 0u);
            EXPECT_EQ(rows.TableDemand, 0u);
        }
    }

    // The snapshots copy the RESOLVED scene and the scratch is single-sample, so MSAA
    // multiplies none of the diffusion's bytes: a 4x cell reports the native numbers.
    TEST_F(SkinMemoryEvidence, TheDiffusionTargetsAreSingleSampleUnderMsaa)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetSkin(true);
        const SkinRows native = ReadSkinRows(RenderCell(RenderingPath::Deferred, "SkinMemory", "Deferred_Native"));
        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 4u;
        const SkinRows msaa = ReadSkinRows(RenderCell(RenderingPath::Deferred, "SkinMemory", "Deferred_Msaa4"));
        const u32 samples = Renderer3D::GetRendererSettings().Deferred.MSAASampleCount;
        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
        Renderer3D::ApplyRendererSettings();
        if (samples != 4u)
            GTEST_SKIP() << "the device gave " << samples << " samples, not 4";

        EXPECT_EQ(msaa.Scratch, native.Scratch);
        EXPECT_EQ(msaa.Snapshots, native.Snapshots);
        EXPECT_EQ(msaa.UboDemand, msaa.UboCapacity) << "the diffusion stopped drawing under MSAA";
    }

    // The diffusion's targets follow the scene band, so an upscale shrinks them by the
    // render scale; the deferred table and the UBO do not move.
    TEST_F(SkinMemoryEvidence, TheDiffusionTargetsFollowTheRenderScale)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetSkin(true);
        const SkinRows native = ReadSkinRows(RenderCell(RenderingPath::Deferred, "SkinMemory", "Deferred_Native"));
        Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Performance;
        const SkinRows scaled = ReadSkinRows(RenderCell(RenderingPath::Deferred, "SkinMemory", "Deferred_UpscalePerformance"));

        ASSERT_GT(native.Scratch, 0u);
        EXPECT_GT(scaled.Scratch, 0u) << "the diffusion stopped running under an upscale";
        EXPECT_LT(scaled.Scratch, native.Scratch) << "the scratch did not shrink with the render scale";
        // Same image size for all three: snapshots = scratch x (8 + 4) / 8.
        EXPECT_EQ(scaled.Snapshots * kRgba16fBytes, scaled.Scratch * (kRgba16fBytes + kD24S8Bytes))
            << "the snapshots and the scratch are no longer at the same resolution";
        EXPECT_EQ(scaled.UboCapacity, native.UboCapacity);
        EXPECT_EQ(scaled.TableCapacity, native.TableCapacity);
        std::cout << "[ skin memory ] " << TestRenderBackend::EvidenceName("SkinMemory") << "Deferred_UpscalePerformance: scratch "
                  << scaled.Scratch << " (native " << native.Scratch << ")\n";
    }
} // namespace OloEngine::Tests
