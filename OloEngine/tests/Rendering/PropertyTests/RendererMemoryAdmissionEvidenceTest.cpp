// OLO_TEST_LAYER: plumbing
// =============================================================================
// RendererMemoryAdmissionEvidenceTest.cpp -- issue #1343, on real frames.
//
// RendererMemoryBudgetTest.cpp pins the admission policy against a simulated renderer.
// This file renders the real pipeline under an INJECTED device budget
// (Renderer3D::SetMemoryBudgetOverride, the pressure seam) and checks what the policy
// promises against what the tracker measured and what the frame shows:
//
//   * every path x MSAA x upscale cell: a budget nothing fits lands on the minimum
//     complete image and says so; a budget between that and the request lands on a level
//     that fits, with a stable reason; relief climbs back to the request after a stable
//     run and the resident bytes return to where they started;
//   * every one of those frames is a COMPLETE image: each cube covers the pixels its
//     projection says it does, and the gaps between them are background. The reference is
//     the camera's own projection, not another frame;
//   * the request survives a reduction: the settings accessors still return what was
//     asked for, and the report carries requested beside realized;
//   * a resize under pressure never allocates the targets it was not admitted for (with
//     OLO_RENDERER_MEMORY_ADMISSION_OFF as the control that it would have);
//   * a reload under pressure is charged while its old storage retires;
//   * a rung that resizes one history chain resets that chain and no other;
//   * two views of one frame share the budget.
//
// Each cell writes MemoryAdmission_<Backend>_<Cell>_<Phase>.png (and
// MemoryAdmissionOff_... for the unreduced control) to OloEditor/assets/tests/visual/,
// and its report to OloEditor/assets/tests/memory/. Runs in both processes: the GL one
// and OloEngine-Tests --olo-rhi=vulkan.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReportJson.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererMemoryBudget.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanTrackedAllocation.h"
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace OloEngine;        // NOLINT(google-build-using-namespace)
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    constexpr u64 kMiB = 1024ull * 1024ull;
    constexpr u32 kSize = 512;
    // The camera looks down -Z from z = kCameraDistance with a 60 degree vertical field of
    // view: the frame is 2 * d * tan(30) world units across.
    constexpr f32 kCameraDistance = 8.0f;
    constexpr f32 kCubeSize = 1.2f;
    constexpr std::array<f32, 3> kCubeX = { -2.0f, 0.0f, 2.0f };

    // Short clocks, and a reserve and band sized for a 512x512 target: the defaults are
    // tuned for a real frame rate and a multi-gigabyte budget.
    constexpr u32 kRecoveryFrames = 6;
    constexpr u32 kSettleFrames = 4;
    constexpr u64 kReserveBytes = 256ull * 1024ull;
    // Frames that outlast any transition here: a staged descent, its settle run, the
    // recovery run back up and the two-frame retirement of what it replaced.
    constexpr u32 kConvergeFrames = 3u * (kRecoveryFrames + kSettleFrames) + 8u;

    [[nodiscard]] MemoryAdmissionPolicy EvidencePolicy()
    {
        MemoryAdmissionPolicy policy;
        policy.ReserveBytes = kReserveBytes;
        policy.ReserveFraction = 0.0f;
        policy.RecoveryBandFraction = 0.0005f;
        policy.RecoveryFrames = kRecoveryFrames;
        policy.SettleFrames = kSettleFrames;
        return policy;
    }

    [[nodiscard]] u64 LiveBytes()
    {
        const auto& tracker = RendererMemoryTracker::GetInstance();
        const u64 resident = tracker.GetGpuResidentBytes();
        return resident - std::min(resident, tracker.GetGpuRetiringBytes());
    }

    [[nodiscard]] u64 OwnerLiveGpuBytes(const RendererMemoryReport& report, std::string_view owner)
    {
        u64 bytes = 0;
        for (const auto& row : report.Owners)
            if (row.Owner.ToView() == owner)
                bytes += row.GpuLiveBytes;
        return bytes;
    }

    void WriteImage(const std::string& name, const std::vector<u8>& rgba, const u32 width, const u32 height)
    {
        const fs::path dir = fs::path("assets") / "tests" / "visual";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string file = (dir / (name + ".png")).string();
        // Rows arrive bottom-up; the PNG is written top-down.
        const sizet rowBytes = static_cast<sizet>(width) * 4u;
        std::vector<u8> topDown(rgba.size());
        for (u32 y = 0; y < height; ++y)
            std::copy_n(rgba.data() + static_cast<sizet>(height - 1u - y) * rowBytes, rowBytes, topDown.data() + static_cast<sizet>(y) * rowBytes);
        EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(width), static_cast<int>(height), 4, topDown.data(), static_cast<int>(rowBytes)), 0)
            << "could not write " << file;
    }

    void WriteReport(const std::string& name)
    {
        const fs::path dir = fs::path("assets") / "tests" / "memory";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out(dir / (name + ".json"), std::ios::binary | std::ios::trunc);
        out << RendererMemoryReportToJson(RendererMemoryTracker::GetInstance().BuildReport()).dump(2) << '\n';
    }

    // Mean 8-bit colour of a square centred on (cx, cy).
    [[nodiscard]] glm::vec3 MeanColour(const std::vector<u8>& rgba, const u32 width, const u32 cx, const u32 cy, const u32 halfExtent)
    {
        glm::dvec3 sum(0.0);
        u32 count = 0;
        for (u32 y = cy - halfExtent; y < cy + halfExtent; ++y)
        {
            for (u32 x = cx - halfExtent; x < cx + halfExtent; ++x)
            {
                const sizet at = (static_cast<sizet>(y) * width + x) * 4u;
                sum += glm::dvec3(rgba[at], rgba[at + 1], rgba[at + 2]);
                ++count;
            }
        }
        return glm::vec3(sum / static_cast<f64>(count));
    }

    [[nodiscard]] f32 Distance(const glm::vec3& a, const glm::vec3& b)
    {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
    }

    // Where the scene's own projection puts a world X on the frame's horizontal midline.
    [[nodiscard]] u32 ColumnOf(const f32 worldX, const u32 width)
    {
        const f32 halfWidth = kCameraDistance * std::tan(glm::radians(30.0f));
        return static_cast<u32>((0.5f + 0.5f * worldX / halfWidth) * static_cast<f32>(width));
    }

    // "Complete image", against the camera's projection: every cube covers its own
    // footprint with something that is not background, and the gaps between the cubes and
    // beside them ARE background. A frame with a pass missing fails the first half; a frame
    // smeared or cleared to one colour fails the second.
    void ExpectCompleteImage(const std::vector<u8>& rgba, const u32 width, const u32 height, const std::string& what)
    {
        SCOPED_TRACE(what);
        ASSERT_EQ(rgba.size(), static_cast<sizet>(width) * height * 4u);
        const u32 row = height / 2u;
        const f32 halfWidth = kCameraDistance * std::tan(glm::radians(30.0f));
        // A probe well inside a cube's front face (the face is nearer than the centre, so
        // its footprint is a little wider than this).
        const u32 probe = std::max(2u, static_cast<u32>(0.25f * kCubeSize / (2.0f * halfWidth) * static_cast<f32>(width)));

        const glm::vec3 edge = MeanColour(rgba, width, probe + 2u, row, probe);
        for (const f32 gapX : { -1.0f, 1.0f })
        {
            const glm::vec3 gap = MeanColour(rgba, width, ColumnOf(gapX, width), row, probe);
            EXPECT_LT(Distance(gap, edge), 24.0f) << "the gap at x=" << gapX << " is not background: (" << gap.x << ", " << gap.y << ", "
                                                  << gap.z << ") against (" << edge.x << ", " << edge.y << ", " << edge.z << ")";
        }
        for (const f32 cubeX : kCubeX)
        {
            const glm::vec3 cube = MeanColour(rgba, width, ColumnOf(cubeX, width), row, probe);
            EXPECT_GT(Distance(cube, edge), 40.0f) << "no geometry where the cube at x=" << cubeX << " projects: (" << cube.x << ", " << cube.y
                                                   << ", " << cube.z << ") against background (" << edge.x << ", " << edge.y << ", " << edge.z << ")";
            EXPECT_GT(cube.x + cube.y + cube.z, 60.0f) << "the cube at x=" << cubeX << " is unlit";
        }
    }

    struct Cell
    {
        const char* Name;
        RenderingPath Path;
        u32 Msaa;
        UpscaleMode Upscale;
        UpscalerTechnique Technique;
    };

    void PrintTo(const Cell& cell, std::ostream* out)
    {
        *out << cell.Name;
    }

    class ScopedAdmissionOff
    {
      public:
        ScopedAdmissionOff() : m_Previous(Levers::RendererMemoryAdmissionOff())
        {
            Levers::SetRendererMemoryAdmissionOff(true);
        }
        ~ScopedAdmissionOff()
        {
            Levers::SetRendererMemoryAdmissionOff(m_Previous);
        }
        ScopedAdmissionOff(const ScopedAdmissionOff&) = delete;
        ScopedAdmissionOff& operator=(const ScopedAdmissionOff&) = delete;

      private:
        bool m_Previous;
    };

    class RendererMemoryAdmissionFixture : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);

            m_SavedPolicy = Renderer3D::GetMemoryAdmissionPolicy();
            Renderer3D::SetMemoryAdmissionPolicy(EvidencePolicy());
            Renderer3D::SetMemoryBudgetOverride(std::nullopt);
            Renderer3D::ResetMemoryAdmission();
            m_AdmissionConfigured = true;

            auto& settings = Renderer3D::GetRendererSettings();
            settings.ShowGrid = false;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.AutoExposureEnabled = false;
            post.Exposure = 1.0f;

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, kCameraDistance };
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 100.0f);
            cam.Camera.SetViewportSize(kSize, kSize);
            m_Camera = camera;

            Entity sun = GetScene().CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.15f, -0.3f, -1.0f));
            light.m_Color = glm::vec3(1.0f);
            light.m_Intensity = 3.0f;

            const Ref<Mesh> cubeMesh = MeshPrimitives::CreateCube();
            ASSERT_TRUE(cubeMesh);
            for (const f32 x : kCubeX)
            {
                Entity cube = GetScene().CreateEntity("Cube");
                cube.AddComponent<MeshComponent>(cubeMesh->GetMeshSource());
                auto& transform = cube.GetComponent<TransformComponent>();
                transform.Translation = { x, 0.0f, 0.0f };
                transform.Scale = glm::vec3(kCubeSize);
            }
        }

        void TearDown() override
        {
            if (m_AdmissionConfigured)
            {
                Renderer3D::SetMemoryBudgetOverride(std::nullopt);
                Renderer3D::SetMemoryAdmissionPolicy(m_SavedPolicy);
                Renderer3D::ResetMemoryAdmission();
            }
            RendererAttachedTest::TearDown();
        }

        // The request of a cell. Everything the ladder can take is on, so each rung has
        // something to remove where the path supports it. Returns the MSAA the device gave.
        u32 Configure(const Cell& cell)
        {
            RendererSettings& settings = Renderer3D::GetRendererSettings();
            settings.Path = cell.Path;
            settings.Deferred.MSAASampleCount = cell.Msaa;
            PostProcessSettings& post = Renderer3D::GetPostProcessSettings();
            post.Upscale = cell.Upscale;
            post.Technique = cell.Technique;
            post.BloomEnabled = true;
            if (cell.Path == RenderingPath::Deferred)
            {
                post.SSGIEnabled = true;
                post.SSGIHalfResolution = false;
                post.SSREnabled = true;
            }
            Renderer3D::ApplyRendererSettings();
            return settings.Path == RenderingPath::Deferred ? settings.Deferred.MSAASampleCount : 1u;
        }

        void Capture(std::vector<u8>& out)
        {
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(out, width, height)) << "no composited frame to read back";
            ASSERT_EQ(width, kSize);
            ASSERT_EQ(height, kSize);
        }

        // A budget nothing here can reach: the request is admitted as asked.
        static void InjectAmpleBudget()
        {
            Renderer3D::SetMemoryBudgetOverride(RendererMemoryTracker::GetInstance().GetGpuResidentBytes() + 16384ull * kMiB);
        }

        Entity m_Camera;
        MemoryAdmissionPolicy m_SavedPolicy;
        bool m_AdmissionConfigured = false;
    };

    class RendererMemoryAdmissionEvidence : public RendererMemoryAdmissionFixture, public ::testing::WithParamInterface<Cell>
    {
    };
} // namespace

// -------------------------------------------------------------------------------------
// Criteria 1 and 3, per cell: pressure -> a reduced COMPLETE image with a stable reason,
// the floor kept and reported, recovery back to the request, the request never edited.
// -------------------------------------------------------------------------------------
TEST_P(RendererMemoryAdmissionEvidence, InjectedPressureAdmitsACompleteReducedImageAndRecovers)
{
    OLO_ENSURE_GPU_OR_SKIP();
    const Cell& cell = GetParam();
    if (cell.Technique == UpscalerTechnique::Temporal && !TemporalUpscalerUsable())
        GTEST_SKIP() << "no temporal upscaler in this process (FSR2 is not built or not usable on this backend)";

    const u32 msaa = Configure(cell);
    if (msaa != cell.Msaa)
        GTEST_SKIP() << "the device returned " << msaa << " samples for a " << cell.Msaa << "x cell";
    const std::string name = TestRenderBackend::EvidenceName("MemoryAdmission") + cell.Name;
    const std::string controlName = TestRenderBackend::EvidenceName("MemoryAdmissionOff") + cell.Name;
    auto& tracker = RendererMemoryTracker::GetInstance();

    // --- A. The request, under a budget it fits ------------------------------------------
    InjectAmpleBudget();
    RunFrames(kConvergeFrames);
    {
        const MemoryAdmissionSnapshot& admitted = Renderer3D::GetMemoryAdmission();
        ASSERT_EQ(admitted.Reason, MemoryAdmissionReason::Admitted);
        ASSERT_FALSE(admitted.IsReduced());
        ASSERT_EQ(admitted.BudgetSource, MemoryBudgetSource::Injected);
    }
    const u64 liveRequested = LiveBytes();
    std::vector<u8> control;
    Capture(control);
    WriteImage(controlName, control, kSize, kSize);
    ExpectCompleteImage(control, kSize, kSize, "the unreduced control");

    // --- B. A budget nothing fits: the minimum complete image, kept and reported ---------
    const u64 degradationsBefore = Renderer3D::GetMemoryAdmission().Degradations;
    Renderer3D::SetMemoryBudgetOverride(kMiB);
    RunFrames(kConvergeFrames);
    const MemoryAdmissionSnapshot floor = Renderer3D::GetMemoryAdmission();
    EXPECT_EQ(floor.Reason, MemoryAdmissionReason::OverBudgetAtFloor);
    EXPECT_EQ(floor.Level, floor.FloorLevel);
    ASSERT_EQ(floor.Views.Num(), 1);
    EXPECT_TRUE(floor.Views[0].IsReduced());
    EXPECT_EQ(floor.Views[0].Realized.MsaaSamples, 1u);
    EXPECT_FALSE(floor.Views[0].Realized.Has(MemoryTechnique::Bloom));
    EXPECT_FALSE(floor.Views[0].Realized.Has(MemoryTechnique::SSGI));
    EXPECT_FALSE(floor.Views[0].Realized.Has(MemoryTechnique::SSR));
    // The minimum complete image keeps the path and both resolutions.
    EXPECT_EQ(floor.Views[0].Realized.Path, cell.Path);
    EXPECT_EQ(floor.Views[0].Realized.DisplayWidth, kSize);
    EXPECT_EQ(floor.Views[0].Realized.SceneWidth, floor.Views[0].Requested.SceneWidth);
    // The request is never edited: outside a frame the settings are what was asked for.
    EXPECT_EQ(Renderer3D::GetRendererSettings().Deferred.MSAASampleCount, cell.Msaa);
    EXPECT_TRUE(Renderer3D::GetPostProcessSettings().BloomEnabled);
    EXPECT_EQ(floor.Views[0].Requested.MsaaSamples, cell.Path == RenderingPath::Deferred ? cell.Msaa : 1u);
    EXPECT_TRUE(floor.Views[0].Requested.Has(MemoryTechnique::Bloom));
    // It got there in a bounded number of moves and stopped.
    EXPECT_LE(floor.Degradations - degradationsBefore, static_cast<u64>(kMemoryLadderRungCount));
    EXPECT_GT(floor.TicksAtLevel, static_cast<u64>(kSettleFrames));

    const u64 liveFloor = LiveBytes();
    EXPECT_LE(liveFloor, liveRequested) << "the minimum image holds more than the request";
    const RendererMemoryReport floorReport = tracker.BuildReport();
    ASSERT_TRUE(floorReport.Admission.has_value()) << "the memory report carries no admission block";
    EXPECT_EQ(floorReport.Admission->Reason, MemoryAdmissionReason::OverBudgetAtFloor);
    EXPECT_TRUE(floorReport.Admission->Views[0].IsReduced()) << "the report claims the requested quality after a reduction";
    if (cell.Path == RenderingPath::Deferred)
    {
        // What is ALLOCATED is what was realized: the G-Buffer the table holds is the 1x one.
        const u64 expected = EstimateGBufferBytes(floor.Views[0].Realized);
        const u64 held = OwnerLiveGpuBytes(floorReport, "GBuffer");
        EXPECT_GE(held, expected);
        EXPECT_LE(held, expected + expected / 8u) << "the G-Buffer still holds more than one sample per pixel";
    }
    WriteReport(name + "_Floor");
    std::vector<u8> floorImage;
    Capture(floorImage);
    WriteImage(name + "_Floor", floorImage, kSize, kSize);
    ExpectCompleteImage(floorImage, kSize, kSize, "the minimum complete image");

    // --- C. Relief: back to the request after a stable run, and to the same bytes --------
    const u64 recoveriesBefore = Renderer3D::GetMemoryAdmission().Recoveries;
    InjectAmpleBudget();
    RunFrames(2);
    EXPECT_TRUE(Renderer3D::GetMemoryAdmission().IsReduced()) << "the climb did not wait for a stable run";
    EXPECT_EQ(Renderer3D::GetMemoryAdmission().Reason, MemoryAdmissionReason::RecoveryHold);
    RunFrames(kConvergeFrames);
    {
        const MemoryAdmissionSnapshot& recovered = Renderer3D::GetMemoryAdmission();
        EXPECT_EQ(recovered.Reason, MemoryAdmissionReason::Admitted);
        EXPECT_FALSE(recovered.IsReduced());
        EXPECT_EQ(recovered.Recoveries - recoveriesBefore, 1u) << "the recovery took more than one reallocation";
        EXPECT_EQ(recovered.FailedRecoveries, 0u);
    }
    const u64 liveRecovered = LiveBytes();
    EXPECT_EQ(liveRecovered, liveRequested) << "a pressure round trip left "
                                            << static_cast<i64>(liveRecovered) - static_cast<i64>(liveRequested) << " bytes behind";
    std::vector<u8> recoveredImage;
    Capture(recoveredImage);
    WriteImage(name + "_Recovered", recoveredImage, kSize, kSize);
    ExpectCompleteImage(recoveredImage, kSize, kSize, "the recovered image");

    std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << cell.Name << ": requested " << liveRequested << " B, floor "
              << liveFloor << " B (level " << floor.Level << ", rungs 0x" << std::hex << floor.ActiveRungMask << std::dec << "), recovered "
              << liveRecovered << " B\n";

    // --- D. A budget between the floor and the request: a level that FITS, twice the same --
    const u64 range = liveRequested - std::min(liveRequested, liveFloor);
    if (range < 4u * kReserveBytes)
    {
        // A forward path holds almost nothing the ladder can give back at this size: the
        // floor and the request are within a reserve of each other, so there is no budget
        // strictly between them to test. The floor and recovery phases above are the cell.
        std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << cell.Name << ": reducible range " << range
                  << " B is inside the reserve; no intermediate budget exists\n";
        return;
    }
    const u64 budget = liveFloor + range / 2u + kReserveBytes;
    struct Outcome
    {
        u32 Level = 0;
        u32 Rungs = 0;
        MemoryAdmissionReason Reason = MemoryAdmissionReason::Admitted;
        u64 Live = 0;
    };
    std::array<Outcome, 2> outcomes;
    for (u32 pass = 0; pass < 2u; ++pass)
    {
        SCOPED_TRACE("pass " + std::to_string(pass));
        tracker.BeginPeakWindow();
        const u64 residentAtInjection = tracker.GetGpuResidentBytes();
        Renderer3D::SetMemoryBudgetOverride(budget);
        RunFrames(1);
        const u64 predictedPeak = Renderer3D::GetMemoryAdmission().PredictedPeakBytes;
        RunFrames(kConvergeFrames);

        const MemoryAdmissionSnapshot pressured = Renderer3D::GetMemoryAdmission();
        outcomes[pass] = { pressured.Level, pressured.ActiveRungMask, pressured.Reason, LiveBytes() };
        EXPECT_TRUE(pressured.IsReduced());
        EXPECT_EQ(pressured.Reason, MemoryAdmissionReason::DeviceBudget);
        EXPECT_LE(outcomes[pass].Live, budget - kReserveBytes) << "the admitted level does not fit the budget";
        EXPECT_EQ(pressured.FailedRecoveries, 0u);
        // Bounded peak: what was resident never passed the larger of what was there when
        // the pressure arrived, the governor's own prediction, and the budget.
        const u64 peak = tracker.BuildReport().Gpu.WindowPeakBytes;
        const u64 bound = std::max({ residentAtInjection, predictedPeak, budget });
        EXPECT_LE(peak, bound + bound / 50u) << "resident bytes peaked at " << peak << " against a bound of " << bound;

        if (pass == 0u)
        {
            WriteReport(name + "_Pressure");
            std::vector<u8> pressureImage;
            Capture(pressureImage);
            WriteImage(name + "_Pressure", pressureImage, kSize, kSize);
            ExpectCompleteImage(pressureImage, kSize, kSize, "the image under pressure");
            std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << cell.Name << ": budget " << budget << " B -> level "
                      << pressured.Level << " of " << pressured.FloorLevel << " (" << ToString(pressured.Reason) << ", rungs 0x" << std::hex
                      << pressured.ActiveRungMask << std::dec << "), live " << outcomes[pass].Live << " B, peak " << peak << " B, predicted peak "
                      << predictedPeak << " B\n";
        }

        InjectAmpleBudget();
        RunFrames(kConvergeFrames);
        EXPECT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced());
    }
    EXPECT_EQ(outcomes[0].Level, outcomes[1].Level) << "the same pressure landed on a different level";
    EXPECT_EQ(outcomes[0].Rungs, outcomes[1].Rungs);
    EXPECT_EQ(outcomes[0].Reason, outcomes[1].Reason);
    EXPECT_EQ(outcomes[0].Live, outcomes[1].Live);
}

INSTANTIATE_TEST_SUITE_P(
    AllCells, RendererMemoryAdmissionEvidence,
    ::testing::Values(Cell{ "Forward", RenderingPath::Forward, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial },
                      Cell{ "ForwardPlus", RenderingPath::ForwardPlus, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial },
                      Cell{ "Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial },
                      Cell{ "Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off, UpscalerTechnique::Spatial },
                      Cell{ "Deferred_Msaa4_UpscalePerformance", RenderingPath::Deferred, 4u, UpscaleMode::Performance, UpscalerTechnique::Spatial },
                      Cell{ "Deferred_TemporalUpscaleQuality", RenderingPath::Deferred, 1u, UpscaleMode::Quality, UpscalerTechnique::Temporal }),
    [](const ::testing::TestParamInfo<Cell>& info)
    { return std::string(info.param.Name); });

namespace
{
    class RendererMemoryAdmissionTransitions : public RendererMemoryAdmissionFixture
    {
      protected:
        void ConfigureDeferredMsaa4()
        {
            m_Msaa = Configure(Cell{ "Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off, UpscalerTechnique::Spatial });
        }
        u32 m_Msaa = 1;
    };
} // namespace

// -------------------------------------------------------------------------------------
// Criterion 2: a resize is admitted BEFORE its targets are allocated.
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, AResizeUnderPressureNeverAllocatesTheTargetsItWasNotAdmittedFor)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ConfigureDeferredMsaa4();
    if (m_Msaa != 4u)
        GTEST_SKIP() << "the device returned " << m_Msaa << " samples";
    auto& tracker = RendererMemoryTracker::GetInstance();
    constexpr u32 kSmall = 256;

    // What a 4x G-Buffer at the large size costs: the allocation a resize that ignored
    // the budget would make.
    MemoryQualityConfig large;
    large.Path = RenderingPath::Deferred;
    large.SceneWidth = large.DisplayWidth = kSize;
    large.SceneHeight = large.DisplayHeight = kSize;
    large.MsaaSamples = 4u;
    const u64 unadmittedGBuffer = EstimateGBufferBytes(large);

    const auto resizeAndMeasure = [&](const bool admissionOff) -> u64
    {
        std::optional<ScopedAdmissionOff> off;
        if (admissionOff)
            off.emplace();
        ResizeRenderTarget(kSmall, kSmall);
        InjectAmpleBudget();
        RunFrames(kConvergeFrames);
        const u64 residentSmall = tracker.GetGpuResidentBytes();
        // Room for half of that G-Buffer on top of what the small frame holds: the large
        // frame fits at fewer samples, never at four.
        Renderer3D::SetMemoryBudgetOverride(residentSmall + unadmittedGBuffer / 2u);
        RunFrames(kSettleFrames + 2u);
        tracker.BeginPeakWindow();
        ResizeRenderTarget(kSize, kSize);
        RunFrames(kSettleFrames);
        return tracker.BuildReport().Gpu.WindowPeakBytes - std::min(tracker.BuildReport().Gpu.WindowPeakBytes, residentSmall);
    };

    const u64 readmissionsBefore = Renderer3D::GetMemoryAdmission().Readmissions;
    const u64 admittedGrowth = resizeAndMeasure(/*admissionOff=*/false);
    const MemoryAdmissionSnapshot admitted = Renderer3D::GetMemoryAdmission();
    EXPECT_GT(admitted.Readmissions, readmissionsBefore) << "the resize was not re-admitted";
    ASSERT_EQ(admitted.Views.Num(), 1);
    EXPECT_EQ(admitted.Views[0].Realized.DisplayWidth, kSize) << "the realized size must be the requested one";
    EXPECT_LT(admitted.Views[0].Realized.MsaaSamples, 4u);
    EXPECT_TRUE(admitted.IsReduced());
    EXPECT_LT(admittedGrowth, unadmittedGBuffer) << "the resize allocated the 4x G-Buffer it was not admitted for";
    std::vector<u8> image;
    Capture(image);
    WriteImage(TestRenderBackend::EvidenceName("MemoryAdmission") + "Deferred_Msaa4_ResizeUnderPressure", image, kSize, kSize);
    ExpectCompleteImage(image, kSize, kSize, "the frame after a resize under pressure");

    // The control: with admission off the same resize DOES allocate it. If this stops
    // holding, the check above measures nothing.
    const u64 unadmittedGrowth = resizeAndMeasure(/*admissionOff=*/true);
    EXPECT_GE(unadmittedGrowth, unadmittedGBuffer) << "the control did not allocate the 4x G-Buffer either";
    std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << "resize 256 -> 512 under pressure: grew " << admittedGrowth
              << " B admitted (level " << admitted.Level << ", " << admitted.Views[0].Realized.MsaaSamples << "x) against " << unadmittedGrowth
              << " B with admission off; a 4x G-Buffer is " << unadmittedGBuffer << " B\n";
}

// -------------------------------------------------------------------------------------
// Criterion 2, hot reload: a reload's old storage is charged until it retires.
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, AReloadUnderPressureIsChargedWhileItsOldStorageRetires)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ConfigureDeferredMsaa4();
    auto& tracker = RendererMemoryTracker::GetInstance();

    TextureSpecification spec;
    spec.Width = 1024;
    spec.Height = 1024;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;
    Ref<Texture2D> asset = Texture2D::Create(spec);
    ASSERT_TRUE(asset && asset->IsLoaded());
    constexpr u64 kOldBytes = 1024ull * 1024ull * 4ull;
    constexpr u64 kNewBytes = 2048ull * 2048ull * 4ull;

    InjectAmpleBudget();
    RunFrames(kConvergeFrames);
    ASSERT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced());
    const u64 liveBefore = LiveBytes();

    // A budget the scene fits with the small texture, and not with the reloaded one.
    const u64 budget = liveBefore + kReserveBytes + (kNewBytes - kOldBytes) / 2u;
    Renderer3D::SetMemoryBudgetOverride(budget);
    RunFrames(kSettleFrames + 2u);
    ASSERT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced());

    // The reload's storage path: a new allocation, the old one handed to the deletion queue.
    asset->Resize(2048, 2048);
    RunFrames(1);
    const MemoryAdmissionSnapshot duringOverlap = Renderer3D::GetMemoryAdmission();
    EXPECT_GE(duringOverlap.RetiringBytes, kOldBytes) << "the reload's old storage was dropped from the charge while the GPU may still read it";
    EXPECT_GE(duringOverlap.ResidentBytes, liveBefore + kNewBytes);

    // The reload pushed the scene past the budget: the renderer gives quality back
    // instead of sitting over it.
    RunFrames(kConvergeFrames);
    const MemoryAdmissionSnapshot pressured = Renderer3D::GetMemoryAdmission();
    EXPECT_TRUE(pressured.IsReduced()) << "a reload took the scene over budget and nothing was reduced";
    EXPECT_NE(pressured.Reason, MemoryAdmissionReason::Admitted);
    EXPECT_TRUE(LiveBytes() <= budget - kReserveBytes || pressured.Reason == MemoryAdmissionReason::OverBudgetAtFloor);
    std::vector<u8> image;
    Capture(image);
    WriteImage(TestRenderBackend::EvidenceName("MemoryAdmission") + "Deferred_Msaa4_ReloadUnderPressure", image, kSize, kSize);
    ExpectCompleteImage(image, kSize, kSize, "the frame after a reload under pressure");

    // The asset goes away: the request comes back.
    asset.Reset();
    RunFrames(kConvergeFrames + 4u);
    EXPECT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced());
    std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << "reload 4 MiB -> 16 MiB under a budget of " << budget
              << " B: retiring " << duringOverlap.RetiringBytes << " B charged, level " << pressured.Level << " (" << ToString(pressured.Reason)
              << ") until the asset was released; tracker peak " << tracker.BuildReport().Gpu.PeakBytes << " B\n";
}

// -------------------------------------------------------------------------------------
// Criterion 4: the history-resolution rung resets the SSGI chain and nothing else.
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, AHistoryResolutionChangeResetsOnlyTheChainItResizes)
{
    OLO_ENSURE_GPU_OR_SKIP();
    Configure(Cell{ "Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial });
    Renderer3D::GetPostProcessSettings().TAAEnabled = true;
    Renderer3D::ApplyRendererSettings();
    InjectAmpleBudget();
    RunFrames(24);

    const auto histories = [](const TemporalHistoryEffect effect)
    {
        std::vector<Renderer3D::ViewRetainedHistory> out;
        for (const auto& history : Renderer3D::GetViewRetainedState(kMainRenderView).Histories)
        {
            if (history.Key.Effect == effect && history.TextureIdentity != 0u)
                out.push_back(history);
        }
        return out;
    };
    // Bytes the registry holds for one effect, from the memory report's capacity row.
    const auto historyBytes = [](const std::string_view category) -> u64
    {
        const RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();
        for (const auto& row : report.Capacity)
        {
            if (row.Owner.ToView() == "TemporalHistory" && row.Category.ToView() == category)
                return row.CapacityBytes.value_or(0u);
        }
        return 0;
    };

    const auto ssgiBefore = histories(TemporalHistoryEffect::SSGI);
    const auto ssrBefore = histories(TemporalHistoryEffect::SSR);
    const auto taaBefore = histories(TemporalHistoryEffect::TAA);
    ASSERT_FALSE(ssgiBefore.empty()) << "SSGI holds no history in this process: the test has no subject";
    ASSERT_FALSE(taaBefore.empty());
    const u64 ssgiBytesBefore = historyBytes("SSGI history");
    const u64 taaBytesBefore = historyBytes("TAA history");
    ASSERT_GT(ssgiBytesBefore, 0u);

    // The governor's history-resolution rung is this one field. Driving it through a
    // budget would also take the pool slack; the field alone isolates the reset scope.
    Renderer3D::GetPostProcessSettings().SSGIHalfResolution = true;
    RunFrames(2);

    const auto ssgiAfter = histories(TemporalHistoryEffect::SSGI);
    ASSERT_EQ(ssgiAfter.size(), ssgiBefore.size());
    EXPECT_EQ(historyBytes("SSGI history"), ssgiBytesBefore / 4u) << "the SSGI histories were not rebuilt at half resolution";
    for (sizet i = 0; i < ssgiAfter.size(); ++i)
    {
        EXPECT_NE(ssgiAfter[i].TextureIdentity, ssgiBefore[i].TextureIdentity)
            << "a resized history kept its texture: the old layout would be read as the new one";
        EXPECT_LE(ssgiAfter[i].Age, 2u) << "the resized history kept accumulating across the change";
    }
    const auto taaAfter = histories(TemporalHistoryEffect::TAA);
    ASSERT_EQ(taaAfter.size(), taaBefore.size());
    EXPECT_EQ(historyBytes("TAA history"), taaBytesBefore);
    for (sizet i = 0; i < taaAfter.size(); ++i)
    {
        EXPECT_EQ(taaAfter[i].TextureIdentity, taaBefore[i].TextureIdentity) << "TAA's history was reallocated by an SSGI resize";
        EXPECT_GT(taaAfter[i].Age, taaBefore[i].Age) << "TAA's lineage was broken by an SSGI resize";
    }
    const auto ssrAfter = histories(TemporalHistoryEffect::SSR);
    ASSERT_EQ(ssrAfter.size(), ssrBefore.size());
    for (sizet i = 0; i < ssrAfter.size(); ++i)
    {
        EXPECT_EQ(ssrAfter[i].TextureIdentity, ssrBefore[i].TextureIdentity) << "SSR's history was reallocated by an SSGI resize";
        EXPECT_GT(ssrAfter[i].Age, ssrBefore[i].Age) << "SSR's lineage was broken by an SSGI resize";
    }
}

// -------------------------------------------------------------------------------------
// Multi-view (#1352): two views of one frame are admitted against one budget.
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, TwoViewsOfOneFrameShareTheBudget)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ConfigureDeferredMsaa4();
    if (m_Msaa != 4u)
        GTEST_SKIP() << "the device returned " << m_Msaa << " samples";

    InjectAmpleBudget();
    RunFrames(kConvergeFrames);
    const u64 liveOneView = LiveBytes();
    const MemoryQualityConfig oneView = Renderer3D::GetMemoryAdmission().Views[0].Realized;
    MemoryQualityConfig oneViewNoMsaa = oneView;
    oneViewNoMsaa.MsaaSamples = 1u;
    const u64 msaaSaving = EstimateGBufferBytes(oneView) - EstimateGBufferBytes(oneViewNoMsaa);

    const RenderViewId second = Renderer3D::CreateView(RenderViewDesc{ .Width = kSize, .Height = kSize, .DebugName = "MemoryAdmissionSecond" });
    ASSERT_FALSE(second.IsMain()) << "Renderer3D::CreateView refused";
    {
        const Renderer3D::ScopedView scope(second);
        Configure(Cell{ "Deferred_Msaa4", RenderingPath::Deferred, 4u, UpscaleMode::Off, UpscalerTechnique::Spatial });
    }
    Scene::SceneViewRequest request;
    request.View = second;
    request.CameraTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, kCameraDistance));
    request.Projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
    request.NearClip = 0.05f;
    request.FarClip = 100.0f;
    const auto runBothViews = [&](const u32 frames)
    {
        for (u32 frame = 0; frame < frames; ++frame)
        {
            GetScene().RequestAdditionalView(request);
            RunFrames(1);
        }
    };

    // Both views as requested, under a budget they fit.
    runBothViews(kConvergeFrames);
    ASSERT_EQ(Renderer3D::GetMemoryAdmission().Views.Num(), 2);
    ASSERT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced());
    const u64 liveTwoViews = LiveBytes();
    ASSERT_GT(liveTwoViews, liveOneView) << "the second view booked nothing";

    // A budget ONE view fits as requested and the two together do not: short by one
    // view's worth of MSAA. The second view's memory is charged to the same budget, so
    // both give samples back.
    const u64 budget = liveTwoViews - msaaSaving;
    ASSERT_GT(budget, liveOneView + kReserveBytes) << "one view alone must fit this budget";
    Renderer3D::SetMemoryBudgetOverride(budget);
    runBothViews(kConvergeFrames);

    const MemoryAdmissionSnapshot twoViews = Renderer3D::GetMemoryAdmission();
    ASSERT_EQ(twoViews.Views.Num(), 2);
    EXPECT_TRUE(twoViews.IsReduced()) << "two views were admitted into a budget sized for less";
    EXPECT_EQ(twoViews.Reason, MemoryAdmissionReason::DeviceBudget);
    EXPECT_EQ(twoViews.Views[0].Realized.MsaaSamples, twoViews.Views[1].Realized.MsaaSamples) << "one ladder level applies to every view";
    EXPECT_LT(twoViews.Views[0].Realized.MsaaSamples, 4u);
    EXPECT_LE(LiveBytes(), budget - kReserveBytes) << "the two views together do not fit the budget they share";
    std::vector<u8> image;
    Capture(image);
    WriteImage(TestRenderBackend::EvidenceName("MemoryAdmission") + "Deferred_Msaa4_TwoViews", image, kSize, kSize);
    ExpectCompleteImage(image, kSize, kSize, "the main view while a second view shares its budget");
    std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << "two views under " << budget << " B: level " << twoViews.Level
              << " (" << ToString(twoViews.Reason) << ", " << twoViews.Views[0].Realized.MsaaSamples << "x each), live " << LiveBytes()
              << " B; as requested " << liveTwoViews << " B, one view alone " << liveOneView << " B\n";

    // The second view goes away (before the fixture restores the main view's settings):
    // the main view fits the same budget as requested again.
    Renderer3D::DestroyView(second);
    RunFrames(kConvergeFrames + 4u);
    EXPECT_FALSE(Renderer3D::GetMemoryAdmission().IsReduced()) << "the main view did not return to its request";
    EXPECT_EQ(Renderer3D::GetMemoryAdmission().Views.Num(), 1);
}

// -------------------------------------------------------------------------------------
// The Vulkan budget itself: VMA re-reads the OS figure on a frame advance and on little
// else, and nothing used to advance it. A steady scene kept the budget it had when it last
// grew a memory block.
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, TheVulkanBudgetIsRefreshedEveryFrame)
{
    OLO_ENSURE_GPU_OR_SKIP();
#if OLO_WITH_VULKAN
    if (!TestRenderBackend::IsVulkanProcess())
        GTEST_SKIP() << "OpenGL reports no device budget; this is the Vulkan process's case";
    const u64 before = GetVmaFrameAdvanceCount();
    RunFrames(8);
    EXPECT_GE(GetVmaFrameAdvanceCount() - before, 8u) << "frames passed without VMA being told: its OS budget is stale";
    // With the budget fresh and no override, Vulkan admits against the device's own figure.
    const MemoryAdmissionSnapshot& snapshot = Renderer3D::GetMemoryAdmission();
    EXPECT_TRUE(snapshot.BudgetSource == MemoryBudgetSource::OsReported || snapshot.BudgetSource == MemoryBudgetSource::AllocatorHeuristic)
        << "budget source " << ToString(snapshot.BudgetSource);
    EXPECT_TRUE(snapshot.BudgetBytes.has_value());
    std::cout << "[ admission ] Vulkan device budget " << snapshot.BudgetBytes.value_or(0) << " B from " << ToString(snapshot.BudgetSource)
              << ", resident " << snapshot.ResidentBytes << " B, reason " << ToString(snapshot.Reason) << "\n";
#else
    GTEST_SKIP() << "built without Vulkan";
#endif
}

// -------------------------------------------------------------------------------------
// A whole-technique fallback only helps if the technique gives its memory back. ReSTIR PT
// holds four path-record pools of 592 bytes a pixel each, and used to keep them after it
// was switched off (until the next resize).
// -------------------------------------------------------------------------------------
TEST_F(RendererMemoryAdmissionTransitions, ReSTIRPTGivesItsPoolsBackWhenItIsSwitchedOff)
{
    OLO_ENSURE_GPU_OR_SKIP();
    Configure(Cell{ "Deferred", RenderingPath::Deferred, 1u, UpscaleMode::Off, UpscalerTechnique::Spatial });
    const auto poolBytes = []() -> u64
    {
        const RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();
        for (const auto& row : report.Capacity)
        {
            if (row.Owner.ToView() == "ReSTIRPTPass")
                return row.CapacityBytes.value_or(0u);
        }
        return 0;
    };

    Renderer3D::GetPostProcessSettings().ReSTIRPT.Enabled = true;
    RunFrames(8);
    const u64 engaged = poolBytes();
    if (engaged == 0u)
        GTEST_SKIP() << "ReSTIR PT allocated nothing in this process: it needs hardware ray queries (Vulkan with a ray-tracing device)";
    EXPECT_GE(engaged, static_cast<u64>(kSize) * kSize * 592ull * 4ull);

    Renderer3D::GetPostProcessSettings().ReSTIRPT.Enabled = false;
    RunFrames(kSettleFrames + 4u);
    EXPECT_EQ(poolBytes(), 0u) << "ReSTIR PT is off and still holds " << engaged << " bytes of path-record pools";
    std::cout << "[ admission ] " << TestRenderBackend::EvidenceBackendPrefix() << "ReSTIR PT pools: " << engaged << " B while on, 0 B once off\n";
}
