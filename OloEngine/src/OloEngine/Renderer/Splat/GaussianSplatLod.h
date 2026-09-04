#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <vector>

namespace OloEngine::GaussianSplat
{
    // Hierarchical splat LOD by MERGING (issue #1039).
    //
    // THE RULE THIS REPLACES. The spike's first LOD knob was a budget: keep the
    // N highest-ranked survivors, ranked on projected area times alpha. It caps
    // frame cost and it does not degrade gracefully, because one splat's
    // contribution is not separable from its neighbours' -- dropping half the
    // splats in a region halves the accumulated opacity of everything they were
    // part of. Measured on the fixture, cutting to a quarter took the faint
    // shell from 103,956 painted pixels to 2,074 (a 98 % loss) while the bright
    // discs kept 84 % of theirs. The cloud lost its BODY, not its detail.
    //
    // Merging fixes that by construction: a coarse level is built to carry the
    // same integrated mass as the group it replaces, so nothing disappears, it
    // only gets blurrier. That is a build-time problem, and this is the build.

    // What one cluster's merge is asked to preserve. All three are moments of
    // the group's density, which is why moment-matching is the fit rather than
    // an approximation of one:
    //   * MASS   sum of alpha * sqrt(det Sigma) -- the integrated opacity, up
    //            to the (2 pi)^{3/2} that cancels on both sides
    //   * MEAN   the mass-weighted centroid
    //   * SECOND the mass-weighted covariance about that centroid, which is the
    //            group's own spread PLUS each member's
    struct ClusterMoments
    {
        f64 Mass = 0.0;
        glm::dvec3 Mean{ 0.0 };
        std::array<f64, 6> Covariance{}; // xx, xy, xz, yy, yz, zz
        glm::dvec3 Color{ 0.0 };         // mass-weighted
    };

    // Moments of a set of splats, in f64 because a coarse level is a sum over
    // thousands of f32 terms and the test that compares two of them wants the
    // difference to be the merge's error, not the accumulation's.
    [[nodiscard]] auto ComputeMoments(std::span<const GpuSplat> splats) -> ClusterMoments;

    // determinant of the symmetric matrix whose upper triangle is
    // (xx, xy, xz, yy, yz, zz).
    [[nodiscard]] auto SymmetricDeterminant(const std::array<f32, 6>& sigma) -> f32;

    // One Gaussian carrying the whole group's moments. Returns false when the
    // group is degenerate (zero mass, or a covariance the record cannot hold)
    // and leaves `out` untouched -- a caller that ignores that would emit a
    // splat that renders as nothing and hide the cluster it came from.
    [[nodiscard]] auto MergeCluster(std::span<const GpuSplat> splats, GpuSplat& out) -> bool;

    struct LodSettings
    {
        // Splats per cluster at each level, so each level is roughly this many
        // times smaller than the one below it. Four keeps a level chain that
        // lines up with the powers of four the budget captures used, which is
        // what makes the before/after comparison like-for-like.
        u32 ClusterSize = 4;

        // Stop coarsening at this many splats. Below it a level is no longer a
        // picture of the scene and the memory saved is irrelevant.
        u32 MinLevelSplats = 128;

        // Hard ceiling on chain length, so a pathological input cannot loop.
        u32 MaxLevels = 8;
    };

    // A chain of progressively coarser clouds. Level 0 is the input, shared by
    // reference-free copy; each later level is built from the one before it.
    class SplatLodChain
    {
      public:
        void Build(const SplatCloud& base, const LodSettings& settings = {});

        [[nodiscard]] auto LevelCount() const -> u32
        {
            return static_cast<u32>(m_Levels.size());
        }
        [[nodiscard]] auto Level(u32 index) const -> const SplatCloud&
        {
            return m_Levels[index];
        }
        [[nodiscard]] auto Empty() const -> bool
        {
            return m_Levels.empty();
        }

        // The FINEST level that fits `maxSplats`, or the coarsest level if none
        // does. This is the whole runtime cost of the scheme: the budget picks
        // a level instead of picking splats.
        [[nodiscard]] auto SelectLevel(u32 maxSplats) const -> u32;

        [[nodiscard]] auto TotalGpuBytes() const -> sizet;

        // Clusters whose merge was refused (see MergeCluster) and which are
        // therefore missing from every coarser level. Reported rather than
        // swallowed: a chain built from a cloud this rejects loses geometry
        // silently, and "the coarse level looks thin" is a bad way to find out.
        // The fixture yields zero, and GaussianSplatLodTest asserts that.
        [[nodiscard]] auto DroppedClusters() const -> u32
        {
            return m_DroppedClusters;
        }

      private:
        std::vector<SplatCloud> m_Levels;
        u32 m_DroppedClusters = 0;
    };

    // Partitions `splats` into clusters of at most `clusterSize` by recursive
    // median split on the widest axis. Returns, for each cluster, the range
    // [begin, end) into `orderOut` -- so the caller reads cluster c as
    // orderOut[offsets[c] .. offsets[c+1]).
    //
    // Deterministic: the split sorts on (coordinate, index), so an axis with
    // many equal coordinates still produces one answer.
    void BuildClusters(std::span<const GpuSplat> splats,
                       u32 clusterSize,
                       std::vector<u32>& orderOut,
                       std::vector<u32>& offsetsOut);
} // namespace OloEngine::GaussianSplat
