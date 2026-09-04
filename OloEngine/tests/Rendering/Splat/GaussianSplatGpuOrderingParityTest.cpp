// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GaussianSplatGpuOrderingParityTest.cpp — the GPU per-view ordering pass
// against its CPU reference (issues #1038, #1043).
//
// WHAT MAKES THIS A PARITY TEST AND NOT A SMOKE TEST. The GPU pass is asserted
// INDEX FOR INDEX against `BuildViewOrdering`, not merely "same set, roughly
// far-to-near". That is possible because both sides order on the PAIR
// (complemented depth key, cloud index), which is a total order: exactly one
// permutation satisfies it, so a correct GPU sort has no freedom to differ.
// A test that only checked the depth ordering would pass with the payload
// permuted among equal depths, which is the shimmer a viewer actually notices.
//
// AND THAT IS WHY THE ASSERTIONS DID NOT MOVE WHEN #1043 REPLACED THE SORT.
// The bitonic network compared the whole pair, so it needed no stability; the
// radix sorts on the key alone and must be STABLE for the same equality to
// hold.
//
// WHAT EACH KIND OF CASE ACTUALLY CATCHES, since it is not what it looks like.
// An LSD radix needs stability to be CORRECT, not merely to be reproducible --
// every pass has to preserve the previous pass's order among equal digits -- so
// a broken per-tile rank fails the distinct-key cases loudly, on the keys
// themselves. `MatchesTheCpuReferenceWhenEveryDepthIsIdentical` is the case
// that catches what the others cannot: with every key bit-identical, ANY
// permutation is correctly sorted by key, so the array still looks right and
// only the payload order can betray a wrong rank. It is the one test whose
// subject is purely the (key, index) tiebreak the frame depends on. Both claims
// were checked by replaying the shader's tile decomposition on the CPU against
// two wrong ranks -- a strided lane assignment and an atomic-style random one.
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
#include <limits>
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
            ASSERT_TRUE(m_Gpu->Initialize()) << "SplatSpike_Cull.comp / SplatSpike_RadixHistogram.comp / "
                                                "SplatSpike_RadixScatter.comp failed to compile";
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

    // PLAIN TESTS, NOT TEST_Fs, ON PURPOSE. The padding functions are pure and
    // they decide how the sort buffers are sized and how much work the sort
    // does -- so they are exactly the contract that should still be checked on
    // a headless runner, where the fixture below skips for want of a GL context
    // and would take these assertions with it.
    //
    // AND THIS IS WHERE ISSUE #1043's FIX IS ACTUALLY PINNED. The measurement
    // further down cannot be asserted -- a GPU time on a dev workstation moves
    // 4x run to run -- but the padding it is a consequence of is arithmetic,
    // deterministic, and reproducible on any machine including CI.
    TEST(GaussianSplatGpuOrderingPadding, RoundsUpToAWholeRadixTile)
    {
        // The radix sorts `count` elements; the only padding is up to a whole
        // 2048-element tile, so the shader needs no bounds test in its inner
        // loop and the cull can keep marking rejects in place.
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(0), 2048u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(1), 2048u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(2048), 2048u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(2049), 4096u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(4096), 4096u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(4097), 6144u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(100000), 100352u);

        // At the ceiling the answer is still representable, and kMaxSplats is a
        // multiple of the tile so it maps to itself.
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(GpuViewOrdering::kMaxSplats),
                  GpuViewOrdering::kMaxSplats);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(GpuViewOrdering::kMaxSplats - 1),
                  GpuViewOrdering::kMaxSplats);

        // Every u32 size derived from the ceiling still fits, which is what the
        // constant is chosen for -- 32 bytes of record and 4 bytes of sort slot
        // per splat, four times over for the key, the payload and their scratch.
        constexpr u64 kRecordBytes = static_cast<u64>(GpuViewOrdering::kMaxSplats) * 32u;
        constexpr u64 kSlotBytes = static_cast<u64>(GpuViewOrdering::kMaxSplats) * 4u;
        EXPECT_LE(kRecordBytes, static_cast<u64>(std::numeric_limits<u32>::max()));
        EXPECT_LE(kSlotBytes, static_cast<u64>(std::numeric_limits<u32>::max()));
    }

    TEST(GaussianSplatGpuOrderingPadding, TheStepFunctionIsGone)
    {
        // THE ACCEPTANCE CRITERION OF ISSUE #1043, in the one form that can be
        // asserted rather than printed.
        //
        // ADR 0018 section 5.2 measured 2,000,000 splats at 1.24 ms and
        // 2,100,000 at 2.16 ms under the bitonic network -- 5 % more splats for
        // 1.7x the time, and nothing about the cloud explained it. What
        // explained it is right here: the network padded to a power of two, so
        // those two counts sorted arrays that differed by 2x. The radix pads to
        // a tile, and the same pair costs 0.69 and 0.72 ms.
        constexpr u32 kJustUnder = 2000000;
        constexpr u32 kJustOver = 2100000;

        EXPECT_EQ(GpuViewOrdering::BitonicPaddedCapacityFor(kJustUnder), 1u << 21);
        EXPECT_EQ(GpuViewOrdering::BitonicPaddedCapacityFor(kJustOver), 1u << 22);

        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(kJustUnder), 2000896u);
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(kJustOver), 2101248u);

        // 5 % more splats now buys at most 5 % more sorted array, against the
        // 2x the network charged. Stated as a bound rather than as the two
        // numbers above so that a future change to the tile size still has to
        // keep the property this issue existed to establish.
        const f64 countRatio = static_cast<f64>(kJustOver) / static_cast<f64>(kJustUnder);
        const f64 paddedRatio = static_cast<f64>(GpuViewOrdering::PaddedCapacityFor(kJustOver)) /
                                static_cast<f64>(GpuViewOrdering::PaddedCapacityFor(kJustUnder));
        EXPECT_LT(paddedRatio, countRatio * 1.01)
            << "the sorted array grew faster than the cloud did: the step function is back";

        // The general form: padding overhead is bounded by one tile, so it is a
        // vanishing fraction of any cloud worth sorting rather than a factor.
        for (const u32 count : { 1000u, 4096u, 100000u, 1500000u, 2000000u, 2100000u, 3000000u, 4000000u,
                                 6000000u, 8000000u, GpuViewOrdering::kMaxSplats })
        {
            const u32 padded = GpuViewOrdering::PaddedCapacityFor(count);
            EXPECT_GE(padded, count) << count;
            EXPECT_LT(padded - count, 2048u) << count;
        }
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
        //
        // THESE EIGHT COUNTS ARE UNCHANGED FROM #1038 BY REQUIREMENT. They are
        // the bitonic network's boundaries, and #1043 kept them exactly so the
        // radix has to clear the bar the sort it replaced cleared, on the same
        // inputs. `MatchesTheCpuReferenceAcrossTheRadixTileSeam` adds the new
        // sort's own seam rather than moving these.
        ViewSettings settings;
        settings.MaxSplats = 0;
        settings.MinScreenExtentPixels = 0.5f;

        for (const u32 count : { 1u, 511u, 512u, 513u, 1024u, 4095u, 4096u, 4097u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 1038u + count);
            ExpectParity(cloud, kParityPoses[0], settings);
        }
    }

    TEST_F(GaussianSplatGpuOrderingTest, MatchesTheCpuReferenceAcrossTheRadixTileSeam)
    {
        // The same idea as the test above, for the boundary the radix
        // introduced: the sort is dispatched one workgroup per 2048-element
        // tile, so a count either side of a tile is where a scatter that
        // mis-ranks its last partial tile shows up.
        ViewSettings settings;
        settings.MaxSplats = 0;
        settings.MinScreenExtentPixels = 0.5f;

        for (const u32 count : { 2047u, 2048u, 2049u, 6143u, 6144u, 6145u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 1043u + count);
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

    TEST_F(GaussianSplatGpuOrderingTest, MatchesTheCpuReferenceWhenTiesSpanManyRadixTiles)
    {
        // THE TIE CASE ABOVE IS 2000 SPLATS, WHICH IS ONE RADIX TILE. That is
        // not a criticism of it -- it is #1038's test and #1043 kept it exactly
        // -- but a single tile can only exercise a rank that is wrong WITHIN a
        // tile. The radix has a second stability obligation the bitonic network
        // never had: equal keys in tile 5 must land behind equal keys in tile 4.
        // That comes from writing the histogram transposed,
        // `hist[bin * numTiles + tile]`, so one scan orders the bins globally
        // and the tiles within each bin.
        //
        // To be straight about what this test adds: a grossly wrong histogram
        // layout already fails the fixture test loudly, because the fixture is
        // two tiles of mostly distinct depths. What is NOT otherwise covered is
        // a large tie GROUP spanning a tile seam, which is the shape a real
        // scan of a flat wall produces and the shape in which a cross-tile rank
        // error stays quiet. Two clouds: one where every depth is identical
        // across three tiles, and one where seventeen depth groups straddle the
        // seams.
        ViewSettings settings;
        settings.MaxSplats = 0;

        const auto makePlanarCloud = [](u32 count, u32 distinctDepths) -> SplatCloud
        {
            std::vector<glm::vec3> positions(count);
            std::vector<glm::vec3> shDc(count, glm::vec3(0.0f));
            std::vector<f32> logit(count, 1.0f);
            std::vector<glm::vec3> logScale(count, glm::vec3(std::log(0.02f)));
            std::vector<glm::vec4> rotation(count, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
            std::mt19937 rng(4243);
            std::uniform_real_distribution<f32> spread(-1.0f, 1.0f);
            for (u32 i = 0; i < count; ++i)
            {
                // Depths drawn from a small discrete set, and the whole cloud
                // kept well inside the frustum at every one of them. Both
                // matter: the groups have to be exact bitwise ties rather than
                // near-ties the two processors could round apart, and no splat
                // may sit within a ULP of a cull threshold or the exact-parity
                // assertions flake for a reason that is not the sort's.
                const f32 z = 0.05f * static_cast<f32>(i % distinctDepths);
                positions[i] = glm::vec3(spread(rng), spread(rng), z);
            }
            SplatCloud cloud;
            cloud.Build(positions, shDc, logit, logScale, rotation);
            return cloud;
        };

        // 6000 splats is two whole tiles plus a partial third.
        ExpectParity(makePlanarCloud(6000, 1), kParityPoses[0], settings);
        ExpectParity(makePlanarCloud(6000, 17), kParityPoses[0], settings);
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

        // The sort's shape, pinned. A four-pass LSD radix is FOUR histograms,
        // four scans and four scatters WHATEVER the cloud is -- that constant
        // is the whole difference from the bitonic network, whose 2 log N grew
        // with the padded array and reached 91 global passes at 2^22. A
        // regression to a per-digit loop that ran more than four times, or to
        // the bitonic path by default, fails here rather than only in a
        // measurement nobody reads.
        const GpuViewOrdering::DispatchCounts dispatches = m_Gpu->LastDispatchCounts();
        std::printf("[splat-gpu] padded=%u dispatches: cull=%u histogram=%u scatter=%u scanCalls=%u\n",
                    m_Gpu->PaddedCapacity(), dispatches.Cull, dispatches.RadixHistogram, dispatches.RadixScatter,
                    dispatches.RadixScanCalls);
        EXPECT_EQ(dispatches.Cull, 1u);
        EXPECT_EQ(dispatches.RadixHistogram, 4u);
        EXPECT_EQ(dispatches.RadixScatter, 4u);
        EXPECT_EQ(dispatches.RadixScanCalls, 4u);
        EXPECT_EQ(dispatches.SortGlobal, 0u) << "the bitonic control ran instead of the radix";
        EXPECT_EQ(dispatches.SortLocal, 0u) << "the bitonic control ran instead of the radix";
    }

    TEST_F(GaussianSplatGpuOrderingTest, TheBitonicControlStillAgreesWithTheCpuReference)
    {
        // The A/B control is only worth measuring against if it is still
        // correct, and it is only still correct if something exercises it: a
        // control that has quietly rotted makes the comparison in
        // MeasureRadixAgainstTheBitonicControl meaningless in the direction
        // that flatters the change.
        ASSERT_TRUE(m_Gpu->SetSortAlgorithm(SortAlgorithm::Bitonic))
            << "SplatSpike_BitonicSort.comp failed to compile";
        ASSERT_EQ(m_Gpu->GetSortAlgorithm(), SortAlgorithm::Bitonic);

        ViewSettings settings;
        settings.MaxSplats = 0;
        settings.MinScreenExtentPixels = 0.5f;

        // 5000 SPLATS, NOT A ROUND NUMBER: the radix pads it to 6144 (three
        // tiles) and the bitonic network to 8192 (a power of two), so the
        // padding assertion below actually proves which sort ran. At 3000 both
        // answer 4096 and it would prove nothing.
        const SplatCloud cloud = MakeParityCloud(5000u, 1043u);
        ExpectParity(cloud, kParityPoses[0], settings);

        const GpuViewOrdering::DispatchCounts dispatches = m_Gpu->LastDispatchCounts();
        EXPECT_EQ(m_Gpu->PaddedCapacity(), 8192u) << "the control must still pad to a power of two";
        EXPECT_EQ(GpuViewOrdering::PaddedCapacityFor(5000u), 6144u) << "and the radix must not";
        EXPECT_EQ(dispatches.RadixHistogram, 0u);
        EXPECT_GT(dispatches.SortGlobal + dispatches.SortLocal, 0u);
    }

    // -------------------------------------------------------------------------
    // Measurement. Printed, never asserted -- a perf assertion on a dev
    // workstation is a flake (oloengine-perf-tests-are-dev-workstation-only).
    // -------------------------------------------------------------------------

    namespace
    {
        // One GL_TIME_ELAPSED bracket around one BuildOrdering, drained.
        [[nodiscard]] f64 TimeOneOrdering(GpuViewOrdering& gpu, const glm::mat4& view, const glm::mat4& projection,
                                          const glm::vec2& viewport, const ViewSettings& settings)
        {
            u32 query = 0;
            ::glCreateQueries(GL_TIME_ELAPSED, 1, &query);
            ::glBeginQuery(GL_TIME_ELAPSED, query);
            gpu.BuildOrdering(view, projection, viewport, settings);
            ::glEndQuery(GL_TIME_ELAPSED);
            ::glFinish();

            u64 nanoseconds = 0;
            ::glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
            ::glDeleteQueries(1, &query);
            return static_cast<f64>(nanoseconds) / 1.0e6;
        }

        [[nodiscard]] glm::mat4 MeasurementView()
        {
            return glm::lookAt(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        }

        // Run the pass until the GPU has been busy for a quarter second, then
        // throw those runs away.
        //
        // ONE WARM-UP DISPATCH IS NOT A WARM-UP ON THIS BOX, and getting that
        // wrong produced a table that read like a real finding. Building a
        // 2 M-splat cloud in a Debug host takes SECONDS, the GPU idles through
        // all of it and drops its clocks, and four adjacent millisecond samples
        // never bring them back -- so 2,000,000 measured at 2.7 ms while
        // 1,500,000 measured at 0.58 ms and 3,000,000 at 0.98 ms, which is an
        // impossible shape for a sort that is linear in the count. The rows
        // that looked slow were the rows whose cloud took longest to build.
        // ADR 0018 section 5.3 recorded the same effect for the raster pass
        // (10.3 ms against 4.3 ms for an identical draw); this is that lesson
        // applied to a sweep whose per-row CPU setup cost varies by 2000x.
        void WarmUp(GpuViewOrdering& gpu, const glm::mat4& view, const glm::mat4& projection,
                    const glm::vec2& viewport, const ViewSettings& settings)
        {
            constexpr auto kWarmDuration = std::chrono::milliseconds(250);
            constexpr int kMaxIterations = 500;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kMaxIterations; ++i)
            {
                gpu.BuildOrdering(view, projection, viewport, settings);
                ::glFinish();
                if (std::chrono::steady_clock::now() - start >= kWarmDuration)
                    break;
            }
        }

        // Minimum and median of a run of adjacent timings. The minimum is the
        // least noisy estimator of the steady state; the median is reported
        // beside it because when the two disagree the box was not idle, and
        // that is the only in-band signal a reader gets.
        struct Timing
        {
            f64 MinMs = 0.0;
            f64 MedianMs = 0.0;
        };
        [[nodiscard]] Timing TimeOrdering(GpuViewOrdering& gpu, const glm::mat4& view, const glm::mat4& projection,
                                          const glm::vec2& viewport, const ViewSettings& settings)
        {
            constexpr int kSamples = 9;
            std::vector<f64> samples;
            samples.reserve(kSamples);
            for (int i = 0; i < kSamples; ++i)
                samples.push_back(TimeOneOrdering(gpu, view, projection, viewport, settings));
            std::sort(samples.begin(), samples.end());
            return { samples.front(), samples[samples.size() / 2] };
        }
    } // namespace

    TEST_F(GaussianSplatGpuOrderingTest, MeasureGpuOrderingCost)
    {
        const glm::mat4 view = MeasurementView();
        const glm::mat4 projection = ParityProjection();
        const glm::vec2 viewport(kWidth, kHeight);
        ViewSettings settings;
        settings.MaxSplats = 0;

        std::printf("\n[splat-gpu] ---- per-view ordering, radix (this machine, not a CI gate) ----\n");
        std::printf("[splat-gpu] %10s %10s %10s %10s %10s %10s\n", "splats", "padded", "drawn", "min-ms",
                    "median-ms", "ns/splat");

        // THE SHAPE OF THIS LIST IS THE MEASUREMENT. 2,000,000 and 2,100,000
        // straddle a power of two, which is what the bitonic network charged
        // 1.7-2.7x for; 1,500,000 and 3,000,000 sit nowhere near one, so
        // together the set separates "large clouds are expensive" (a curve)
        // from "crossing a padding boundary is expensive" (a step), and those
        // imply completely different advice to whoever authors a capture.
        //
        // The ns/splat column is the one to read across rows: a sort that
        // scales with the cloud holds it roughly flat, and a sort that scales
        // with a padded array does not. The median beside the minimum is the
        // honesty column -- when the two disagree by more than a few per cent
        // the box was not idle and the row should not be quoted.
        for (const u32 count : { 4096u, 100000u, 500000u, 1500000u, 2000000u, 2100000u, 3000000u, 4000000u,
                                 6000000u, 8000000u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 77u + count);
            m_Gpu->SetCloud(cloud);

            WarmUp(*m_Gpu, view, projection, viewport, settings);
            const Timing timing = TimeOrdering(*m_Gpu, view, projection, viewport, settings);

            ViewOrdering gpu;
            m_Gpu->ReadbackOrdering(gpu);
            std::printf("[splat-gpu] %10u %10u %10u %10.3f %10.3f %10.3f\n", count, m_Gpu->PaddedCapacity(),
                        gpu.Stats.Drawn, timing.MinMs, timing.MedianMs,
                        timing.MinMs * 1.0e6 / static_cast<f64>(count));
        }
        std::printf("[splat-gpu] ----------------------------------------------------------------\n\n");
    }

    TEST_F(GaussianSplatGpuOrderingTest, MeasureRadixAgainstTheBitonicControl)
    {
        // INTERLEAVED, NOT A-THEN-B, and that is not fastidiousness. The
        // identical dispatch sequence on this box has been measured at 0.506,
        // 1.127 and 2.150 ms across three runs -- a 4x spread from GPU clock
        // state and from whatever else is on the machine. Measuring the whole
        // of A and then the whole of B charges all of that drift to whichever
        // ran second, which is enough to invert a comparison. Alternating
        // A,B,A,B splits it between them; the minimum of each side is then the
        // least noisy estimator of the steady state
        // (gpu-timing-measurement, ADR 0018 section 5.3).
        const glm::mat4 view = MeasurementView();
        const glm::mat4 projection = ParityProjection();
        const glm::vec2 viewport(kWidth, kHeight);
        ViewSettings settings;
        settings.MaxSplats = 0;

        if (!m_Gpu->SetSortAlgorithm(SortAlgorithm::Bitonic))
            GTEST_SKIP() << "the bitonic control shader did not compile on this driver";
        ASSERT_TRUE(m_Gpu->SetSortAlgorithm(SortAlgorithm::Radix));

        std::printf("\n[splat-gpu] ---- radix against the bitonic control, interleaved ----\n");
        std::printf("[splat-gpu] %10s %10s %10s %10s %10s %8s\n", "splats", "radixPad", "bitonicPad", "radix-ms",
                    "bitonic-ms", "speedup");

        // Four counts, each earning its place. 4,096 is the fixture, and it is
        // the one place the radix could legitimately LOSE -- its fixed cost is
        // nine dispatches plus four scans whatever the cloud, against a network
        // that at this size is only six global steps and twelve local ones, so
        // the honest table has to include the size that flatters the old sort.
        // 500 k is the control that straddles nothing, so a run in which every
        // row moved together would read as a machine effect rather than a win.
        // And the 2.0 M / 2.1 M pair either side of 2^21 is the whole reason
        // issue #1043 exists.
        for (const u32 count : { 4096u, 500000u, 2000000u, 2100000u })
        {
            const SplatCloud cloud = MakeParityCloud(count, 77u + count);

            f64 bestRadixMs = 1.0e30;
            f64 bestBitonicMs = 1.0e30;
            u32 radixPadded = 0;
            u32 bitonicPadded = 0;

            for (int block = 0; block < 3; ++block)
            {
                ASSERT_TRUE(m_Gpu->SetSortAlgorithm(SortAlgorithm::Radix));
                if (block == 0)
                    m_Gpu->SetCloud(cloud); // uploads once; SetSortAlgorithm only resizes the sort buffers
                WarmUp(*m_Gpu, view, projection, viewport, settings);
                radixPadded = m_Gpu->PaddedCapacity();
                bestRadixMs =
                    std::min(bestRadixMs, TimeOrdering(*m_Gpu, view, projection, viewport, settings).MinMs);

                ASSERT_TRUE(m_Gpu->SetSortAlgorithm(SortAlgorithm::Bitonic));
                WarmUp(*m_Gpu, view, projection, viewport, settings);
                bitonicPadded = m_Gpu->PaddedCapacity();
                bestBitonicMs =
                    std::min(bestBitonicMs, TimeOrdering(*m_Gpu, view, projection, viewport, settings).MinMs);
            }

            std::printf("[splat-gpu] %10u %10u %10u %10.3f %10.3f %7.2fx\n", count, radixPadded, bitonicPadded,
                        bestRadixMs, bestBitonicMs, bestBitonicMs / bestRadixMs);
        }
        std::printf("[splat-gpu] ----------------------------------------------------------------\n\n");

        // Leave the object on the algorithm that ships, so a later test in this
        // process inherits the pass rather than the control.
        ASSERT_TRUE(m_Gpu->SetSortAlgorithm(SortAlgorithm::Radix));
    }
} // namespace OloEngine::Tests
