// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GaussianSplatGpuOrderingParityTest.cpp — the GPU per-view ordering pass
// against its CPU reference (issue #1038).
//
// WHAT MAKES THIS A PARITY TEST AND NOT A SMOKE TEST. The GPU pass is asserted
// INDEX FOR INDEX against `BuildViewOrdering`, not merely "same set, roughly
// far-to-near". That is possible because both sides order on the PAIR
// (complemented depth key, cloud index), which is a total order: exactly one
// permutation satisfies it, so a correct GPU sort has no freedom to differ.
// A test that only checked the depth ordering would pass with the payload
// permuted among equal depths, which is the shimmer a viewer actually notices.
//
// The cull stats are compared field by field for the same reason: a cull that
// dropped the right NUMBER of splats for the wrong REASON would otherwise look
// identical, and the reason is what tells a future reader which of the four
// tests drifted.
//
// SKIPs cleanly with no GL 4.6 context. Run from OloEditor/ so the compute
// shaders resolve.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"
#include "OloEngine/Renderer/Splat/GaussianSplatGpuOrdering.h"
#include "OloEngine/Renderer/Splat/GaussianSplatView.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;
    using namespace OloEngine::GaussianSplat;

    namespace
    {
        constexpr f32 kWidth = 1280.0f;
        constexpr f32 kHeight = 720.0f;

        [[nodiscard]] fs::path ParityFixturePath()
        {
            static constexpr const char* kRelative = "assets/tests/splat/fixture_discs.ply";
            const std::array<fs::path, 2> candidates{ fs::path(kRelative), fs::path("OloEditor") / kRelative };
            for (const fs::path& candidate : candidates)
            {
                if (fs::exists(candidate))
                    return candidate;
            }
            return {};
        }

        [[nodiscard]] glm::mat4 ParityProjection()
        {
            return glm::perspective(glm::radians(45.0f), kWidth / kHeight, 0.05f, 100.0f);
        }

        // NOTHING IN THIS CLOUD SITS NEAR A CULL THRESHOLD, and that is the
        // point rather than laziness. The CPU and the GPU evaluate the same
        // expressions on different hardware, so a splat whose projected extent
        // is within a ULP of the LOD threshold can legitimately fall on
        // opposite sides and the exact-parity assertions below would flake.
        // Sigmas, opacities and positions are therefore drawn from discrete
        // values far from every threshold; the thresholds themselves are
        // exercised by the fixture poses, where they fire on the whole cloud at
        // once rather than on a boundary case.
        [[nodiscard]] SplatCloud MakeParityCloud(u32 count, u32 seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<f32> unit(-1.0f, 1.0f);
            constexpr std::array<f32, 3> kSigmas{ 0.01f, 0.02f, 0.04f };
            constexpr std::array<f32, 4> kLogits{ -30.0f, -3.0f, 0.0f, 3.0f }; // the first is fully transparent
            std::vector<glm::vec3> positions(count);
            std::vector<glm::vec3> shDc(count);
            std::vector<f32> logit(count);
            std::vector<glm::vec3> logScale(count);
            std::vector<glm::vec4> rotation(count);
            for (u32 i = 0; i < count; ++i)
            {
                positions[i] = glm::vec3(unit(rng), unit(rng), unit(rng)) * 2.0f;
                shDc[i] = glm::vec3(unit(rng), unit(rng), unit(rng));
                logit[i] = kLogits[i % kLogits.size()];
                logScale[i] = glm::vec3(std::log(kSigmas[i % kSigmas.size()]));
                rotation[i] = glm::vec4(unit(rng), unit(rng), unit(rng), unit(rng));
            }
            SplatCloud cloud;
            cloud.Build(positions, shDc, logit, logScale, rotation);
            return cloud;
        }

        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            glm::vec3 Target;
            glm::vec3 Up;
        };

        // Poses chosen so each of the four cull reasons actually fires at least
        // once across the set: nothing is proven by a pass where every splat
        // survives.
        constexpr std::array<Pose, 4> kParityPoses{ {
            { "Front", glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
            { "Inside", glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
            { "Away", glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f, 0.0f, 12.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
            { "Distant", glm::vec3(0.0f, 0.0f, 300.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
        } };
    } // namespace

    class GaussianSplatGpuOrderingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            OLO_ENSURE_GPU_OR_SKIP();

            m_Gpu = std::make_unique<GpuViewOrdering>();
            ASSERT_TRUE(m_Gpu->Initialize())
                << "SplatSpike_Cull.comp / SplatSpike_BitonicSort.comp failed to compile";
        }

        void TearDown() override
        {
            m_Gpu.reset();
        }

        // Runs both paths for one pose and asserts they agree completely.
        void ExpectParity(const SplatCloud& cloud, const Pose& pose, const ViewSettings& settings)
        {
            const glm::mat4 view = glm::lookAt(pose.Eye, pose.Target, pose.Up);
            const glm::mat4 projection = ParityProjection();
            const glm::vec2 viewport(kWidth, kHeight);

            ViewOrdering cpu;
            BuildViewOrdering(cloud, view, projection, viewport, settings, cpu);

            m_Gpu->SetCloud(cloud);
            m_Gpu->BuildOrdering(view, projection, viewport, settings);
            ViewOrdering gpu;
            m_Gpu->ReadbackOrdering(gpu);

            std::printf("[splat-gpu] pose=%-8s cpu drawn=%6u near=%5u frustum=%5u small=%5u faint=%5u\n", pose.Name,
                        cpu.Stats.Drawn, cpu.Stats.BehindNearPlane, cpu.Stats.FrustumCulled, cpu.Stats.TooSmall,
                        cpu.Stats.TooFaint);

            EXPECT_EQ(gpu.Stats.Total, cpu.Stats.Total) << pose.Name;
            EXPECT_EQ(gpu.Stats.Drawn, cpu.Stats.Drawn) << pose.Name;
            EXPECT_EQ(gpu.Stats.BehindNearPlane, cpu.Stats.BehindNearPlane) << pose.Name;
            EXPECT_EQ(gpu.Stats.FrustumCulled, cpu.Stats.FrustumCulled) << pose.Name;
            EXPECT_EQ(gpu.Stats.TooSmall, cpu.Stats.TooSmall) << pose.Name;
            EXPECT_EQ(gpu.Stats.TooFaint, cpu.Stats.TooFaint) << pose.Name;

            ASSERT_EQ(gpu.Indices.size(), cpu.Indices.size()) << pose.Name;
            for (sizet i = 0; i < cpu.Indices.size(); ++i)
                ASSERT_EQ(gpu.Indices[i], cpu.Indices[i]) << pose.Name << " diverged at draw slot " << i;
        }

        std::unique_ptr<GpuViewOrdering> m_Gpu;
    };

    TEST_F(GaussianSplatGpuOrderingTest, PaddingIsAPowerOfTwoAndAtLeastOneTile)
    {
        // The bitonic network is only defined on a power-of-two array, and its
        // shared-memory pass owns a 512-element tile, so a cloud smaller than a
        // tile still has to be padded up to one.
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(0), 512u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(1), 512u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(512), 512u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(513), 1024u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(4096), 4096u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(4097), 8192u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(100000), 131072u);
    }

    TEST_F(GaussianSplatGpuOrderingTest, MatchesTheCpuReferenceOnTheFixtureFromEveryPose)
    {
        const fs::path path = ParityFixturePath();
        ASSERT_FALSE(path.empty()) << "fixture_discs.ply not found from " << fs::current_path().string();
        SplatCloud cloud;
        ASSERT_TRUE(cloud.LoadPly(path).Ok);

        // The GPU pass deliberately implements no budget: #1039 replaces
        // selection with level choice, so the ceiling is applied by picking a
        // LOD level before this pass ever runs. Parity is therefore asserted
        // with the budget lifted on both sides.
        ViewSettings settings;
        settings.MaxSplats = 0; // no cap

        for (const Pose& pose : kParityPoses)
            ExpectParity(cloud, pose, settings);
    }

    TEST_F(GaussianSplatGpuOrderingTest, MatchesTheCpuReferenceOnAwkwardCounts)
    {
        // Counts that straddle the padding boundary and the sort tile: one
        // below, exactly on, one above. A network that mishandles its pad
        // silently drops or duplicates the elements at the seam.
        ViewSettings settings;
        settings.MaxSplats = 0;
        settings.MinScreenExtentPixels = 0.5f;

        for (const u32 count : { 1u, 511u, 512u, 513u, 1024u, 4095u, 4096u, 4097u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 1038u + count);
            ExpectParity(cloud, kParityPoses[0], settings);
        }
    }

    TEST_F(GaussianSplatGpuOrderingTest, MatchesTheCpuReferenceWhenEveryDepthIsIdentical)
    {
        // The tie case, which is where an unstable sort and a total order stop
        // agreeing. Every splat sits on a plane perpendicular to the view, so
        // every depth key is bit-identical and only the payload tiebreak
        // decides the order.
        constexpr u32 kCount = 2000;
        std::vector<glm::vec3> positions(kCount);
        std::vector<glm::vec3> shDc(kCount, glm::vec3(0.0f));
        std::vector<f32> logit(kCount, 1.0f);
        std::vector<glm::vec3> logScale(kCount, glm::vec3(std::log(0.02f)));
        std::vector<glm::vec4> rotation(kCount, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        std::mt19937 rng(4242);
        std::uniform_real_distribution<f32> spread(-1.0f, 1.0f);
        for (u32 i = 0; i < kCount; ++i)
            positions[i] = glm::vec3(spread(rng), spread(rng), 0.0f);

        SplatCloud cloud;
        cloud.Build(positions, shDc, logit, logScale, rotation);

        ViewSettings settings;
        settings.MaxSplats = 0;
        ExpectParity(cloud, kParityPoses[0], settings);
    }

    TEST_F(GaussianSplatGpuOrderingTest, TheIndirectDrawCountIsWrittenByTheGpu)
    {
        // The acceptance criterion that keeps a readback off the frame path:
        // the instance count reaches the draw through the indirect buffer, so
        // it must be there and it must equal the survivor count.
        const fs::path path = ParityFixturePath();
        ASSERT_FALSE(path.empty());
        SplatCloud cloud;
        ASSERT_TRUE(cloud.LoadPly(path).Ok);

        const glm::mat4 view =
            glm::lookAt(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        ViewSettings settings;
        settings.MaxSplats = 0;

        m_Gpu->SetCloud(cloud);
        m_Gpu->BuildOrdering(view, ParityProjection(), glm::vec2(kWidth, kHeight), settings);

        ViewOrdering gpu;
        m_Gpu->ReadbackOrdering(gpu);
        EXPECT_GT(gpu.Stats.Drawn, 0u);

        // The dispatch count is 1 cull + 2 log N sort passes, not log^2 N; if
        // the shared-memory mode regressed to one dispatch per step this jumps
        // by an order of magnitude and the measurement in ADR 0018 stops
        // meaning what it says.
        const GpuViewOrdering::DispatchCounts dispatches = m_Gpu->LastDispatchCounts();
        std::printf("[splat-gpu] padded=%u dispatches: cull=%u sortGlobal=%u sortLocal=%u\n",
                    m_Gpu->PaddedCapacity(), dispatches.Cull, dispatches.SortGlobal, dispatches.SortLocal);
        EXPECT_EQ(dispatches.Cull, 1u);
        EXPECT_LT(dispatches.SortGlobal + dispatches.SortLocal, 64u);
    }

    // -------------------------------------------------------------------------
    // Measurement. Printed, never asserted -- a perf assertion on a dev
    // workstation is a flake (oloengine-perf-tests-are-dev-workstation-only).
    // -------------------------------------------------------------------------

    TEST_F(GaussianSplatGpuOrderingTest, MeasureGpuOrderingCost)
    {
        const glm::mat4 view =
            glm::lookAt(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = ParityProjection();
        const glm::vec2 viewport(kWidth, kHeight);
        ViewSettings settings;
        settings.MaxSplats = 0;

        std::printf("\n[splat-gpu] ---- per-view ordering, GPU (this machine, not a CI gate) ----\n");
        std::printf("[splat-gpu] %10s %10s %10s %12s\n", "splats", "padded", "drawn", "gpu-ms");

        for (const u32 count : { 4096u, 100000u, 500000u, 2000000u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 77u + count);
            m_Gpu->SetCloud(cloud);

            // Warm up: the first dispatch of a freshly sized buffer pays for
            // allocation and shader residency, not for the work.
            m_Gpu->BuildOrdering(view, projection, viewport, settings);
            ::glFinish();

            // Minimum of four, for the reason ADR 0018 section 5.3 records: the
            // absolute time moves by up to 2x with GPU clock state, and the
            // minimum is the least noisy estimator of the steady state.
            f64 bestMs = 1.0e30;
            for (int i = 0; i < 4; ++i)
            {
                u32 query = 0;
                ::glCreateQueries(GL_TIME_ELAPSED, 1, &query);
                ::glBeginQuery(GL_TIME_ELAPSED, query);
                m_Gpu->BuildOrdering(view, projection, viewport, settings);
                ::glEndQuery(GL_TIME_ELAPSED);
                ::glFinish();

                u64 nanoseconds = 0;
                ::glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
                ::glDeleteQueries(1, &query);
                bestMs = std::min(bestMs, static_cast<f64>(nanoseconds) / 1.0e6);
            }

            ViewOrdering gpu;
            m_Gpu->ReadbackOrdering(gpu);
            std::printf("[splat-gpu] %10u %10u %10u %12.3f\n", count, m_Gpu->PaddedCapacity(), gpu.Stats.Drawn,
                        bestMs);
        }
        std::printf("[splat-gpu] ----------------------------------------------------------------\n\n");
    }
} // namespace OloEngine::Tests
