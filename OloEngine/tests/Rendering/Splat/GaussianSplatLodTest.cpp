// OLO_TEST_LAYER: L1
// =============================================================================
// GaussianSplatLodTest.cpp — CPU contracts for hierarchical splat LOD by
// merging (issue #1039).
//
// The property that makes merging the right answer, and selection the wrong
// one, is MASS PRESERVATION: a coarse level has to carry the same integrated
// opacity as the level it replaces, so the scene gets blurrier rather than
// thinner. That is a numeric statement about moments, so it is asserted here
// rather than eyeballed on a capture -- the captures in
// GaussianSplatVisualEvidenceTest show what it looks like, but they cannot
// tell a 5 % mass loss from a 40 % one.
//
// Every expected value comes from the definition of the moment being matched,
// not from running the code: mass is sum(alpha * sqrt(det Sigma)), the mean is
// the mass-weighted centroid, and the second moment about that centroid is the
// group's spread PLUS each member's own covariance (the parallel-axis term).
//
// No GPU: runs everywhere, including headless CI.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"
#include "OloEngine/Renderer/Splat/GaussianSplatLod.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <random>
#include <vector>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;
    using namespace OloEngine::GaussianSplat;

    namespace
    {
        [[nodiscard]] fs::path LodFixturePath()
        {
            static constexpr const char* kRelative = "assets/tests/splat/fixture_discs.ply";
            const std::array<fs::path, 2> candidates{ fs::path("OloEditor") / kRelative, fs::path(kRelative) };
            for (const fs::path& candidate : candidates)
            {
                if (fs::exists(candidate))
                    return candidate;
            }
            fs::path dir = fs::current_path();
            for (int i = 0; i < 6 && !dir.empty(); ++i)
            {
                if (const fs::path candidate = dir / "OloEditor" / kRelative; fs::exists(candidate))
                    return candidate;
                dir = dir.parent_path();
            }
            return {};
        }

        // One splat, built through the same encode path the importer uses so
        // the test drives production code rather than hand-packed bits.
        [[nodiscard]] GpuSplat MakeSplat(const glm::vec3& position, f32 sigma, f32 alpha, const glm::vec3& color)
        {
            SplatCloud cloud;
            const std::array<glm::vec3, 1> positions{ position };
            const std::array<glm::vec3, 1> shDc{ (color - 0.5f) / kShC0 };
            const std::array<f32, 1> logit{ std::log(alpha / (1.0f - alpha)) };
            const std::array<glm::vec3, 1> logScale{ glm::vec3(std::log(sigma)) };
            const std::array<glm::vec4, 1> rotation{ glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) };
            cloud.Build(positions, shDc, logit, logScale, rotation);
            return cloud.Splats()[0];
        }

        [[nodiscard]] SplatCloud MakeRandomCloud(u32 count, u32 seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<f32> unit(-1.0f, 1.0f);
            std::uniform_real_distribution<f32> alpha(0.2f, 0.9f);
            std::vector<glm::vec3> positions(count);
            std::vector<glm::vec3> shDc(count);
            std::vector<f32> logit(count);
            std::vector<glm::vec3> logScale(count, glm::vec3(std::log(0.03f)));
            std::vector<glm::vec4> rotation(count, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
            for (u32 i = 0; i < count; ++i)
            {
                positions[i] = glm::vec3(unit(rng), unit(rng), unit(rng));
                shDc[i] = glm::vec3(unit(rng), unit(rng), unit(rng));
                const f32 a = alpha(rng);
                logit[i] = std::log(a / (1.0f - a));
            }
            SplatCloud cloud;
            cloud.Build(positions, shDc, logit, logScale, rotation);
            return cloud;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The merge fit.
    // -------------------------------------------------------------------------

    TEST(GaussianSplatMerge, MergingOneSplatIsTheIdentity)
    {
        const GpuSplat original = MakeSplat(glm::vec3(1.0f, 2.0f, 3.0f), 0.05f, 0.7f, glm::vec3(0.4f, 0.5f, 0.6f));
        const std::array<GpuSplat, 1> group{ original };

        GpuSplat merged{};
        ASSERT_TRUE(MergeCluster(group, merged));
        EXPECT_EQ(merged.ColorOpacity, original.ColorOpacity);
        EXPECT_EQ(merged.CovXXXY, original.CovXXXY);
        EXPECT_EQ(merged.CovXZYY, original.CovXZYY);
        EXPECT_EQ(merged.CovYZZZ, original.CovYZZZ);
        EXPECT_NEAR(merged.Position.x, original.Position.x, 1e-6f);
    }

    TEST(GaussianSplatMerge, MergedCovarianceCarriesTheGroupSpreadNotJustTheMemberSpread)
    {
        // Two identical small splats a metre apart. The merged Gaussian must be
        // wide enough to cover BOTH -- its variance along the separation axis is
        // the members' own variance plus the squared half-separation
        // (parallel-axis). A merge that averaged only the members' covariances
        // would return the original tiny sigma and the coarse level would be a
        // scatter of dots with a hole between them.
        constexpr f32 kSigma = 0.05f;
        constexpr f32 kHalfSeparation = 0.5f;
        const std::array<GpuSplat, 2> group{
            MakeSplat(glm::vec3(-kHalfSeparation, 0.0f, 0.0f), kSigma, 0.5f, glm::vec3(0.5f)),
            MakeSplat(glm::vec3(kHalfSeparation, 0.0f, 0.0f), kSigma, 0.5f, glm::vec3(0.5f)),
        };

        GpuSplat merged{};
        ASSERT_TRUE(MergeCluster(group, merged));

        // Centroid of two equal masses is the midpoint.
        EXPECT_NEAR(merged.Position.x, 0.0f, 1e-4f);

        const std::array<f32, 6> cov = UnpackCovariance(merged.CovXXXY, merged.CovXZYY, merged.CovYZZZ);
        const f32 expectedXX = kSigma * kSigma + kHalfSeparation * kHalfSeparation;
        EXPECT_NEAR(cov[0], expectedXX, 0.02f * expectedXX);

        // The axes with no separation keep the members' own variance.
        EXPECT_NEAR(cov[3], kSigma * kSigma, 0.05f * kSigma * kSigma);
        EXPECT_NEAR(cov[5], kSigma * kSigma, 0.05f * kSigma * kSigma);
    }

    TEST(GaussianSplatMerge, MergePreservesMassMeanAndColour)
    {
        // A spread-out group of differing sizes, opacities and colours: the
        // case where every term of the fit has to be right at once.
        std::vector<GpuSplat> group{
            MakeSplat(glm::vec3(0.0f, 0.0f, 0.0f), 0.04f, 0.80f, glm::vec3(0.9f, 0.1f, 0.1f)),
            MakeSplat(glm::vec3(0.3f, 0.1f, -0.2f), 0.09f, 0.30f, glm::vec3(0.1f, 0.8f, 0.2f)),
            MakeSplat(glm::vec3(-0.2f, 0.4f, 0.1f), 0.06f, 0.55f, glm::vec3(0.2f, 0.3f, 0.9f)),
            MakeSplat(glm::vec3(0.1f, -0.3f, 0.35f), 0.05f, 0.45f, glm::vec3(0.6f, 0.6f, 0.2f)),
        };

        const ClusterMoments before = ComputeMoments(group);
        ASSERT_GT(before.Mass, 0.0);

        GpuSplat merged{};
        ASSERT_TRUE(MergeCluster(group, merged));

        const std::array<GpuSplat, 1> mergedGroup{ merged };
        const ClusterMoments after = ComputeMoments(mergedGroup);

        // Mass, to within the 8-bit opacity quantisation the record forces.
        EXPECT_NEAR(after.Mass, before.Mass, 0.02 * before.Mass);

        // Mean.
        EXPECT_NEAR(after.Mean.x, before.Mean.x, 1e-3);
        EXPECT_NEAR(after.Mean.y, before.Mean.y, 1e-3);
        EXPECT_NEAR(after.Mean.z, before.Mean.z, 1e-3);

        // Second moment about the mean: this is the property the parallel-axis
        // term exists for, and it must survive the half-precision packing.
        for (sizet i = 0; i < 6; ++i)
        {
            const f64 expected = before.Covariance[i];
            EXPECT_NEAR(after.Covariance[i], expected, 0.03 * std::abs(expected) + 1e-6)
                << "covariance term " << i;
        }

        // Mass-weighted colour.
        EXPECT_NEAR(after.Color.r, before.Color.r, 1.0 / 255.0);
        EXPECT_NEAR(after.Color.g, before.Color.g, 1.0 / 255.0);
        EXPECT_NEAR(after.Color.b, before.Color.b, 1.0 / 255.0);
    }

    TEST(GaussianSplatMerge, DegenerateGroupsAreRejectedRatherThanEmitted)
    {
        GpuSplat merged{};
        EXPECT_FALSE(MergeCluster(std::span<const GpuSplat>{}, merged)) << "an empty group is not a splat";

        // Fully transparent members carry no mass, so there is nothing to
        // preserve and no defensible splat to emit. Emitting one anyway would
        // put an invisible record in every coarse level.
        const std::array<GpuSplat, 2> transparent{
            MakeSplat(glm::vec3(0.0f), 0.05f, 0.001f, glm::vec3(0.5f)),
            MakeSplat(glm::vec3(0.1f), 0.05f, 0.001f, glm::vec3(0.5f)),
        };
        GpuSplat fromTransparent{};
        EXPECT_FALSE(MergeCluster(transparent, fromTransparent));
    }

    // -------------------------------------------------------------------------
    // Clustering.
    // -------------------------------------------------------------------------

    TEST(GaussianSplatClustering, PartitionsEverySplatExactlyOnceAndRespectsTheTarget)
    {
        const SplatCloud cloud = MakeRandomCloud(1000, 1039u);

        std::vector<u32> order;
        std::vector<u32> offsets;
        BuildClusters(cloud.Splats(), 4, order, offsets);

        ASSERT_GE(offsets.size(), 2u);
        EXPECT_EQ(offsets.front(), 0u);
        EXPECT_EQ(offsets.back(), cloud.Count());

        std::vector<u32> seen(cloud.Count(), 0u);
        for (const u32 index : order)
        {
            ASSERT_LT(index, cloud.Count());
            ++seen[index];
        }
        for (const u32 timesSeen : seen)
            EXPECT_EQ(timesSeen, 1u) << "a splat landed in two clusters, or none";

        for (sizet c = 0; c + 1 < offsets.size(); ++c)
        {
            const u32 size = offsets[c + 1] - offsets[c];
            EXPECT_GT(size, 0u);
            EXPECT_LE(size, 4u) << "cluster " << c << " exceeded the target size";
        }
    }

    TEST(GaussianSplatClustering, IsDeterministicIncludingOnCoincidentSplats)
    {
        // A plane of coincident coordinates is where a median split without an
        // index tiebreak stops being a function of its input.
        std::vector<glm::vec3> positions(64, glm::vec3(1.0f, 2.0f, 3.0f));
        std::vector<glm::vec3> shDc(64, glm::vec3(0.0f));
        std::vector<f32> logit(64, 0.0f);
        std::vector<glm::vec3> logScale(64, glm::vec3(std::log(0.02f)));
        std::vector<glm::vec4> rotation(64, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        SplatCloud cloud;
        cloud.Build(positions, shDc, logit, logScale, rotation);

        std::vector<u32> orderA;
        std::vector<u32> offsetsA;
        std::vector<u32> orderB;
        std::vector<u32> offsetsB;
        BuildClusters(cloud.Splats(), 4, orderA, offsetsA);
        BuildClusters(cloud.Splats(), 4, orderB, offsetsB);

        EXPECT_EQ(orderA, orderB);
        EXPECT_EQ(offsetsA, offsetsB);
    }

    // -------------------------------------------------------------------------
    // The level chain.
    // -------------------------------------------------------------------------

    TEST(GaussianSplatLodChain, CoarsensGeometricallyAndTerminates)
    {
        const fs::path path = LodFixturePath();
        ASSERT_FALSE(path.empty());
        SplatCloud base;
        ASSERT_TRUE(base.LoadPly(path).Ok);

        SplatLodChain chain;
        chain.Build(base);

        ASSERT_GE(chain.LevelCount(), 3u) << "4096 splats at 4:1 should give at least 4096/1024/256";
        EXPECT_EQ(chain.Level(0).Count(), base.Count());

        u32 previous = chain.Level(0).Count();
        for (u32 level = 1; level < chain.LevelCount(); ++level)
        {
            const u32 count = chain.Level(level).Count();
            EXPECT_LT(count, previous) << "level " << level << " did not coarsen";
            // 4:1 clustering, so a level should be near a quarter of the one
            // below; the bound is loose because a median split cannot always
            // fill every cluster.
            EXPECT_LE(count, previous / 2u) << "level " << level;
            previous = count;
        }

        // The whole chain costs a third more than the base level, which is the
        // geometric series 1 + 1/4 + 1/16 + ... The number is quoted in the ADR.
        EXPECT_LT(chain.TotalGpuBytes(), base.GpuBytes() * 2);
    }

    TEST(GaussianSplatLodChain, EveryLevelCarriesTheSameMass)
    {
        // THE contract of #1039. A coarse level may lose a little mass to the
        // opacity clamp (a merged splat cannot be more than fully opaque), so
        // the bound is one-sided and generous downward; what it must never do
        // is GAIN mass, which would mean the scene gets brighter as it
        // coarsens.
        const fs::path path = LodFixturePath();
        ASSERT_FALSE(path.empty());
        SplatCloud base;
        ASSERT_TRUE(base.LoadPly(path).Ok);

        SplatLodChain chain;
        chain.Build(base);

        const f64 baseMass = ComputeMoments(chain.Level(0).Splats()).Mass;
        ASSERT_GT(baseMass, 0.0);

        for (u32 level = 1; level < chain.LevelCount(); ++level)
        {
            const ClusterMoments moments = ComputeMoments(chain.Level(level).Splats());
            const f64 ratio = moments.Mass / baseMass;
            std::printf("[splat-lod] level=%u splats=%6u mass ratio=%.4f\n", level, chain.Level(level).Count(),
                        ratio);
            EXPECT_LE(ratio, 1.05) << "level " << level << " gained mass";
            EXPECT_GE(ratio, 0.70) << "level " << level << " lost mass the merge was supposed to preserve";
        }
    }

    TEST(GaussianSplatLodChain, LevelSelectionIsTheBudget)
    {
        const fs::path path = LodFixturePath();
        ASSERT_FALSE(path.empty());
        SplatCloud base;
        ASSERT_TRUE(base.LoadPly(path).Ok);

        SplatLodChain chain;
        chain.Build(base);

        // A budget larger than the cloud selects the finest level; a budget
        // below the coarsest selects the coarsest rather than failing.
        EXPECT_EQ(chain.SelectLevel(base.Count() * 2), 0u);
        EXPECT_EQ(chain.SelectLevel(0), chain.LevelCount() - 1);

        // Monotone: a tighter budget never selects a finer level.
        u32 previousLevel = 0;
        for (const u32 budget : { 4096u, 2048u, 1024u, 512u, 256u, 64u })
        {
            const u32 level = chain.SelectLevel(budget);
            EXPECT_GE(level, previousLevel) << "budget " << budget;
            if (level < chain.LevelCount() - 1)
                EXPECT_LE(chain.Level(level).Count(), budget) << "budget " << budget << " was exceeded";
            previousLevel = level;
        }
    }

    TEST(GaussianSplatLodChain, AnEmptyOrTinyCloudProducesOneLevelAndStops)
    {
        SplatLodChain empty;
        empty.Build(SplatCloud{});
        EXPECT_EQ(empty.LevelCount(), 1u);
        EXPECT_EQ(empty.SelectLevel(100), 0u);

        // Below MinLevelSplats there is nothing to coarsen, and the loop must
        // notice rather than spin building levels that never shrink.
        const SplatCloud tiny = MakeRandomCloud(16, 7u);
        SplatLodChain chain;
        chain.Build(tiny);
        EXPECT_EQ(chain.LevelCount(), 1u);
    }
} // namespace OloEngine::Tests
