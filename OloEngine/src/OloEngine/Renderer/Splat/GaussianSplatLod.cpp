#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Splat/GaussianSplatLod.h"

#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace OloEngine::GaussianSplat
{
    namespace
    {
        [[nodiscard]] f64 SymmetricDeterminant64(const std::array<f64, 6>& s)
        {
            // (xx, xy, xz, yy, yz, zz)
            return s[0] * (s[3] * s[5] - s[4] * s[4]) - s[1] * (s[1] * s[5] - s[4] * s[2]) +
                   s[2] * (s[1] * s[4] - s[3] * s[2]);
        }

        // A splat's contribution to the group, up to the (2 pi)^{3/2} that
        // appears on both sides of every comparison and therefore cancels:
        // alpha times the volume of its 1-sigma ellipsoid.
        //
        // THE VARIANCE FLOOR IS LOAD-BEARING, not defensive padding. A splat
        // fitted to a flat surface -- which is most of a real scan -- is very
        // oblate, and the record stores its covariance in HALF, whose denormals
        // stop at 6e-8. A thin axis below sigma = 2.4e-4 therefore packs to
        // exactly zero, the determinant is zero, and the splat weighs nothing:
        // it would contribute nothing to the moment fit and be erased by the
        // first coarsening. That is precisely the failure #1039 exists to
        // prevent, and the mass test could not catch it, because the baseline it
        // compares against is computed by this same function.
        //
        // The floor is the smallest thickness the record can represent, so a
        // splat is treated as being exactly as thin as the format allows rather
        // than as having no volume at all.
        constexpr f64 kMinVariance = 6.0e-8;

        // Raises the three diagonal terms to the floor, which is what stops a
        // rank-deficient covariance from having determinant zero. Applied in
        // BOTH places a determinant is taken: to a member when weighing it, and
        // to the MERGED covariance, because a cluster of coplanar splats -- a
        // patch of wall -- is itself rank-deficient in the direction none of its
        // members has any thickness in. Flooring only the members would fix the
        // weighing and still refuse the merge.
        [[nodiscard]] std::array<f64, 6> WithVarianceFloor(const std::array<f64, 6>& sigma)
        {
            std::array<f64, 6> floored = sigma;
            floored[0] = std::max(floored[0], kMinVariance);
            floored[3] = std::max(floored[3], kMinVariance);
            floored[5] = std::max(floored[5], kMinVariance);
            return floored;
        }

        [[nodiscard]] f64 SplatMass(const GpuSplat& splat)
        {
            const std::array<f32, 6> sigma = UnpackCovariance(splat.CovXXXY, splat.CovXZYY, splat.CovYZZZ);
            const std::array<f64, 6> floored =
                WithVarianceFloor({ sigma[0], sigma[1], sigma[2], sigma[3], sigma[4], sigma[5] });

            const f64 determinant = SymmetricDeterminant64(floored);
            if (!(determinant > 0.0))
                return 0.0;
            const f64 alpha = static_cast<f64>((splat.ColorOpacity >> 24) & 0xFFu) / 255.0;
            return alpha * std::sqrt(determinant);
        }

    } // namespace

    auto SymmetricDeterminant(const std::array<f32, 6>& s) -> f32
    {
        return s[0] * (s[3] * s[5] - s[4] * s[4]) - s[1] * (s[1] * s[5] - s[4] * s[2]) +
               s[2] * (s[1] * s[4] - s[3] * s[2]);
    }

    auto ComputeMoments(std::span<const GpuSplat> splats) -> ClusterMoments
    {
        OLO_PROFILE_FUNCTION();

        ClusterMoments moments;
        if (splats.empty())
            return moments;

        // Pass one: mass, mass-weighted mean and colour.
        for (const GpuSplat& splat : splats)
        {
            const f64 mass = SplatMass(splat);
            if (!(mass > 0.0))
                continue;
            const glm::vec4 color = UnpackColorOpacity(splat.ColorOpacity);
            moments.Mass += mass;
            moments.Mean += mass * glm::dvec3(splat.Position);
            moments.Color += mass * glm::dvec3(color.r, color.g, color.b);
        }
        if (!(moments.Mass > 0.0))
            return moments;

        moments.Mean /= moments.Mass;
        moments.Color /= moments.Mass;

        // Pass two: the second moment about that mean. Each member contributes
        // its OWN covariance as well as its displacement -- the parallel-axis
        // term. Leaving the displacement out is the classic mistake and makes a
        // merged splat far too small for a spread-out group.
        for (const GpuSplat& splat : splats)
        {
            const f64 mass = SplatMass(splat);
            if (!(mass > 0.0))
                continue;
            const std::array<f32, 6> sigma = UnpackCovariance(splat.CovXXXY, splat.CovXZYY, splat.CovYZZZ);
            const glm::dvec3 delta = glm::dvec3(splat.Position) - moments.Mean;
            const std::array<f64, 6> outer{ delta.x * delta.x, delta.x * delta.y, delta.x * delta.z,
                                            delta.y * delta.y, delta.y * delta.z, delta.z * delta.z };
            for (sizet i = 0; i < 6; ++i)
                moments.Covariance[i] += mass * (static_cast<f64>(sigma[i]) + outer[i]);
        }
        for (f64& term : moments.Covariance)
            term /= moments.Mass;

        return moments;
    }

    auto MergeCluster(std::span<const GpuSplat> splats, GpuSplat& out) -> bool
    {
        OLO_PROFILE_FUNCTION();

        if (splats.empty())
            return false;

        // A single-splat cluster merges to itself exactly. Going through the
        // moment path would round-trip it through the 8-bit colour and the half
        // covariance a second time for no reason.
        if (splats.size() == 1)
        {
            out = splats[0];
            return true;
        }

        const ClusterMoments moments = ComputeMoments(splats);
        if (!(moments.Mass > 0.0))
            return false;

        const std::array<f64, 6> covariance = WithVarianceFloor(moments.Covariance);
        const f64 determinant = SymmetricDeterminant64(covariance);
        if (!(determinant > 0.0))
            return false;

        // The alpha that makes the merged splat carry the group's mass:
        //     alpha * sqrt(det Sigma) = Mass
        // Clamped to 1, because an opacity above 1 is not representable. When
        // the clamp bites, the merged splat is FAINTER than the group it
        // replaces -- which happens only for a cluster that was already opaque
        // and tightly packed, and is why the level-mass test allows a shortfall
        // but not an excess.
        const f64 alpha = std::min(1.0, moments.Mass / std::sqrt(determinant));

        std::array<f32, 6> packed{};
        for (sizet i = 0; i < 6; ++i)
            packed[i] = static_cast<f32>(covariance[i]);

        out = GpuSplat{};
        out.Position = glm::vec3(moments.Mean);
        out.ColorOpacity = PackColorOpacity(glm::vec3(moments.Color), static_cast<f32>(alpha));
        PackCovariance(packed, out.CovXXXY, out.CovXZYY, out.CovYZZZ);
        return true;
    }

    void BuildClusters(std::span<const GpuSplat> splats,
                       u32 clusterSize,
                       std::vector<u32>& orderOut,
                       std::vector<u32>& offsetsOut)
    {
        OLO_PROFILE_FUNCTION();

        orderOut.resize(splats.size());
        std::iota(orderOut.begin(), orderOut.end(), 0u);
        offsetsOut.clear();

        if (splats.empty())
        {
            offsetsOut.push_back(0u);
            return;
        }

        const u32 target = std::max(clusterSize, 1u);

        // Explicit stack rather than recursion: a degenerate cloud (every splat
        // at one point) splits down to single elements, and 2 M splats would be
        // 21 levels of recursion per leaf.
        struct Range
        {
            u32 Begin;
            u32 End;
        };
        std::vector<Range> pending;
        pending.push_back({ 0u, static_cast<u32>(splats.size()) });

        std::vector<Range> leaves;
        while (!pending.empty())
        {
            const Range range = pending.back();
            pending.pop_back();

            const u32 count = range.End - range.Begin;
            if (count <= target)
            {
                leaves.push_back(range);
                continue;
            }

            // Widest axis of this range's bounds.
            glm::vec3 lo(std::numeric_limits<f32>::max());
            glm::vec3 hi(std::numeric_limits<f32>::lowest());
            for (u32 i = range.Begin; i < range.End; ++i)
            {
                const glm::vec3& position = splats[orderOut[i]].Position;
                lo = glm::min(lo, position);
                hi = glm::max(hi, position);
            }
            const glm::vec3 extent = hi - lo;
            int axis = 0;
            if (extent.y > extent.x)
                axis = 1;
            if (extent.z > extent[axis])
                axis = 2;

            const u32 middle = range.Begin + count / 2;
            std::nth_element(orderOut.begin() + range.Begin, orderOut.begin() + middle, orderOut.begin() + range.End,
                             [&](u32 a, u32 b)
                             {
                                 // No float == anywhere: two > tests separate
                                 // greater, less and equivalent, and the index
                                 // breaks the tie so a plane of coincident
                                 // splats still has one answer.
                                 const f32 pa = splats[a].Position[axis];
                                 const f32 pb = splats[b].Position[axis];
                                 if (pa < pb)
                                     return true;
                                 if (pb < pa)
                                     return false;
                                 return a < b;
                             });

            pending.push_back({ range.Begin, middle });
            pending.push_back({ middle, range.End });
        }

        // nth_element leaves the leaves in stack order; sorting by start offset
        // makes the cluster numbering a function of the geometry alone.
        std::sort(leaves.begin(), leaves.end(), [](const Range& a, const Range& b)
                  { return a.Begin < b.Begin; });

        offsetsOut.reserve(leaves.size() + 1);
        for (const Range& leaf : leaves)
            offsetsOut.push_back(leaf.Begin);
        offsetsOut.push_back(static_cast<u32>(splats.size()));
    }

    void SplatLodChain::Build(const SplatCloud& base, const LodSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        m_Levels.clear();
        m_DroppedClusters = 0;
        m_Levels.push_back(base);
        if (base.Empty())
            return;

        std::vector<u32> order;
        std::vector<u32> offsets;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> shDc;
        std::vector<f32> opacity;
        std::vector<glm::vec3> logScale;
        std::vector<glm::vec4> rotation;

        while (m_Levels.size() < settings.MaxLevels)
        {
            const SplatCloud& previous = m_Levels.back();
            if (previous.Count() <= settings.MinLevelSplats)
                break;

            BuildClusters(previous.Splats(), settings.ClusterSize, order, offsets);
            const sizet clusterCount = offsets.empty() ? 0 : offsets.size() - 1;
            if (clusterCount == 0 || clusterCount >= previous.Count())
                break; // no coarsening happened; another level would loop

            std::vector<GpuSplat> merged;
            merged.reserve(clusterCount);
            std::vector<GpuSplat> members;
            for (sizet c = 0; c < clusterCount; ++c)
            {
                members.clear();
                for (u32 i = offsets[c]; i < offsets[c + 1]; ++i)
                    members.push_back(previous.Splats()[order[i]]);

                GpuSplat mergedSplat;
                if (MergeCluster(members, mergedSplat))
                    merged.push_back(mergedSplat);
                else
                    ++m_DroppedClusters;
            }
            if (merged.empty())
                break;

            m_Levels.push_back(SplatCloud{});
            m_Levels.back().Adopt(std::move(merged));
        }
    }

    auto SplatLodChain::SelectLevel(u32 maxSplats) const -> u32
    {
        if (m_Levels.empty())
            return 0;
        for (u32 level = 0; level < LevelCount(); ++level)
        {
            if (m_Levels[level].Count() <= maxSplats)
                return level;
        }
        return LevelCount() - 1;
    }

    auto SplatLodChain::TotalGpuBytes() const -> sizet
    {
        sizet total = 0;
        for (const SplatCloud& level : m_Levels)
            total += level.GpuBytes();
        return total;
    }
} // namespace OloEngine::GaussianSplat
