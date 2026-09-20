#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomBodyCollider.h"

#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kAxisEpsilon2 = 1.0e-12f;
        /// Power iterations for the dominant eigenvector. A limb is strongly
        /// elongated, so this converges in a handful; the count is fixed rather
        /// than tolerance-driven so the fit is bit-identical on every run, which
        /// is what the cached proxy and its test both rest on.
        constexpr u32 kPowerIterations = 12;

        [[nodiscard]] f32 Percentile(std::vector<f32>& sorted, f32 fraction) noexcept
        {
            // `sorted` really is sorted by the caller. Nearest-rank, not
            // interpolated: one fewer arithmetic path, and the difference is a
            // fraction of a millimetre on a proxy whose radius is authored with
            // a scale slider anyway.
            if (sorted.empty())
            {
                return 0.0f;
            }
            const f32 clamped = std::clamp(fraction, 0.0f, 1.0f);
            const sizet last = sorted.size() - 1u;
            const auto index = static_cast<sizet>(std::lround(clamped * static_cast<f32>(last)));
            return sorted[std::min(index, last)];
        }

        /// The bone with the largest weight at `vertex`, or -1 when every weight
        /// is zero. DOMINANT, not "every bone with any weight": a vertex split
        /// between a forearm and an upper arm belongs to one capsule, and giving
        /// it to both inflates each of them across the elbow.
        [[nodiscard]] i32 DominantBone(const GroomSkinningView& skinning, u32 vertex, u32 boneCount) noexcept
        {
            const auto* ids = reinterpret_cast<const u32*>(reinterpret_cast<const std::byte*>(skinning.BoneIds) +
                                                           static_cast<sizet>(vertex) * skinning.Stride);
            const auto* weights = reinterpret_cast<const f32*>(reinterpret_cast<const std::byte*>(skinning.Weights) +
                                                               static_cast<sizet>(vertex) * skinning.Stride);
            i32 best = -1;
            f32 bestWeight = 0.0f;
            for (u32 i = 0; i < 4u; ++i)
            {
                const f32 weight = weights[i];
                if (!(weight > bestWeight) || !std::isfinite(weight) || ids[i] >= boneCount)
                {
                    continue;
                }
                bestWeight = weight;
                best = static_cast<i32>(ids[i]);
            }
            return best;
        }
    } // namespace

    GroomColliderBuildStats BuildGroomBodyColliders(const GroomSurfaceView& surface,
                                                    const GroomSkinningView& skinning,
                                                    const GroomColliderBuildSettings& settings,
                                                    std::vector<GroomColliderBinding>& outBindings)
    {
        OLO_PROFILE_FUNCTION();

        outBindings.clear();
        GroomColliderBuildStats stats;

        // An unskinned or morph-only body has nothing to carry a capsule. NOT an
        // error, and not a proxy pinned to bone zero, which would be a capsule in
        // the wrong place that looks like a feature working.
        if (!surface.IsUsable() || !skinning.IsSkinned() || skinning.VertexCount != surface.VertexCount)
        {
            return stats;
        }

        const auto boneCount = static_cast<u32>(skinning.Palette.size());
        if (boneCount == 0u)
        {
            return stats;
        }

        // Bucketed by dominant bone in one pass. Indices rather than positions,
        // so a 200k-vertex body costs 800 KB here and not 2.4 MB.
        std::vector<std::vector<u32>> byBone(boneCount);
        for (u32 vertex = 0; vertex < surface.VertexCount; ++vertex)
        {
            const i32 bone = DominantBone(skinning, vertex, boneCount);
            if (bone < 0)
            {
                continue;
            }
            const glm::vec3 position = surface.Position(vertex);
            if (!Math::IsFinite(position))
            {
                // A non-finite vertex would poison the mean and therefore every
                // capsule fitted to this bone. Skipped silently: the surface's
                // own validation owns that complaint, and repeating it here per
                // vertex would be a log flood about somebody else's asset.
                continue;
            }
            byBone[static_cast<u32>(bone)].push_back(vertex);
        }

        std::vector<f32> projections;
        std::vector<f32> radii;
        for (u32 bone = 0; bone < boneCount; ++bone)
        {
            const auto& vertices = byBone[bone];
            if (vertices.empty())
            {
                continue;
            }
            ++stats.BonesConsidered;
            if (vertices.size() < settings.MinVerticesPerBone)
            {
                // A twist helper, an attachment point, an IK target. Fitting a
                // capsule to three vertices gives a shape with no relation to
                // the body it is meant to stand in for.
                ++stats.BonesSkippedTooFewVertices;
                continue;
            }

            glm::dvec3 mean{ 0.0 };
            for (const u32 vertex : vertices)
            {
                mean += glm::dvec3(surface.Position(vertex));
            }
            mean /= static_cast<f64>(vertices.size());
            const glm::vec3 centre{ mean };

            // ── The principal axis ──────────────────────────────────────────
            //
            // A limb is an elongated cloud, so the covariance's dominant
            // eigenvector IS the limb. Power iteration rather than a closed-form
            // 3x3 eigensolver: twelve matrix-vector products, no branches on
            // degenerate discriminants, and the failure mode of a round cloud
            // (no dominant direction) is caught by the length test below rather
            // than by a special case.
            glm::dmat3 covariance{ 0.0 };
            for (const u32 vertex : vertices)
            {
                const glm::dvec3 delta = glm::dvec3(surface.Position(vertex)) - mean;
                covariance += glm::outerProduct(delta, delta);
            }
            covariance /= static_cast<f64>(vertices.size());

            // Seeded off-axis so a cloud whose principal direction happens to be
            // a coordinate axis is not started exactly on an eigenvector of the
            // WRONG eigenvalue, which would never rotate away from it.
            glm::dvec3 axis{ 0.577350269, 0.577350269, 0.577350269 };
            for (u32 iteration = 0; iteration < kPowerIterations; ++iteration)
            {
                const glm::dvec3 next = covariance * axis;
                const f64 length2 = glm::dot(next, next);
                if (length2 <= 1.0e-24)
                {
                    break;
                }
                axis = next / std::sqrt(length2);
            }
            const glm::vec3 direction{ axis };
            if (!Math::IsFinite(direction) || glm::length2(direction) <= kAxisEpsilon2)
            {
                ++stats.BonesSkippedDegenerate;
                continue;
            }

            projections.clear();
            radii.clear();
            projections.reserve(vertices.size());
            radii.reserve(vertices.size());
            for (const u32 vertex : vertices)
            {
                const glm::vec3 delta = surface.Position(vertex) - centre;
                const f32 along = glm::dot(delta, direction);
                projections.push_back(along);
                radii.push_back(glm::length(delta - direction * along));
            }
            std::ranges::sort(projections);
            std::ranges::sort(radii);

            // PERCENTILES, not extrema: a proxy that contains every last vertex
            // of a hand is a sphere around the whole hand, and it pushes the
            // coat off the arm.
            const f32 low = Percentile(projections, settings.AxisLowPercentile);
            const f32 high = Percentile(projections, settings.AxisHighPercentile);
            const f32 radius = Percentile(radii, settings.RadiusPercentile);
            if (!(radius > settings.MinRadius) || !std::isfinite(low) || !std::isfinite(high))
            {
                ++stats.BonesSkippedDegenerate;
                continue;
            }

            GroomColliderBinding binding;
            binding.PointA = centre + direction * low;
            binding.PointB = centre + direction * high;
            binding.Radius = radius;
            binding.BoneIndex = bone;
            outBindings.push_back(binding);
        }

        // ── The cap ─────────────────────────────────────────────────────────
        //
        // The LARGEST by capsule volume are kept, because a coat penetrating a
        // torso is visible from across the room and a coat penetrating a finger
        // is not. Sorted back into bone order afterwards so the output is
        // deterministic and a diff of two proxies is readable.
        if (outBindings.size() > settings.MaxColliders)
        {
            const auto volume = [](const GroomColliderBinding& c)
            {
                const f32 length = glm::length(c.PointB - c.PointA);
                return c.Radius * c.Radius * (length + c.Radius);
            };
            std::ranges::partial_sort(outBindings, outBindings.begin() + settings.MaxColliders,
                                      [&volume](const GroomColliderBinding& a, const GroomColliderBinding& b)
                                      { return volume(a) > volume(b); });
            outBindings.resize(settings.MaxColliders);
            stats.Truncated = true;
        }
        std::ranges::sort(outBindings, [](const GroomColliderBinding& a, const GroomColliderBinding& b)
                          { return a.BoneIndex < b.BoneIndex; });

        stats.CollidersBuilt = static_cast<u32>(outBindings.size());
        return stats;
    }

    void ResolveGroomBodyColliders(std::span<const GroomColliderBinding> bindings,
                                   std::span<const glm::mat4> palette, const glm::mat4& bindingToWorld,
                                   f32 radiusScale, std::vector<GroomCollider>& outColliders)
    {
        OLO_PROFILE_FUNCTION();

        outColliders.clear();
        if (bindings.empty() || palette.empty() || !Math::IsFinite(bindingToWorld) || !std::isfinite(radiusScale))
        {
            return;
        }
        outColliders.reserve(bindings.size());

        for (const GroomColliderBinding& binding : bindings)
        {
            if (binding.BoneIndex >= palette.size())
            {
                // DROPPED, never clamped to a valid bone: a capsule at the wrong
                // limb pushes the coat somewhere plausible and wrong, which is
                // the class of failure this whole subsystem is written against.
                continue;
            }
            const glm::mat4 boneToWorld = bindingToWorld * palette[binding.BoneIndex];
            if (!Math::IsFinite(boneToWorld))
            {
                continue;
            }

            // The mean of the three axis lengths. A capsule has ONE radius, so a
            // non-uniformly scaled bone has no exact answer and this is the
            // approximation, named here rather than discovered later.
            const f32 scale = (glm::length(glm::vec3(boneToWorld[0])) + glm::length(glm::vec3(boneToWorld[1])) +
                               glm::length(glm::vec3(boneToWorld[2]))) /
                              3.0f;

            GroomCollider collider;
            collider.PointA = glm::vec3(boneToWorld * glm::vec4(binding.PointA, 1.0f));
            collider.PointB = glm::vec3(boneToWorld * glm::vec4(binding.PointB, 1.0f));
            collider.Radius = binding.Radius * scale * radiusScale;
            if (!Math::IsFinite(collider.PointA) || !Math::IsFinite(collider.PointB) ||
                !std::isfinite(collider.Radius) || !(collider.Radius > 0.0f))
            {
                continue;
            }
            outColliders.push_back(collider);
        }
    }
} // namespace OloEngine
