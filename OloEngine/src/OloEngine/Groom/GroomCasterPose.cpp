#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomCasterPose.h"

#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OloEngine
{
    u64 GroomCasterPose::CpuBytes() const noexcept
    {
        u64 bytes = static_cast<u64>(Runs.capacity()) * sizeof(GroomCasterRunPose) +
                    static_cast<u64>(Samples.capacity()) * sizeof(GroomCasterPoseSample);
        for (const GroomCasterRunPose& run : Runs)
        {
            bytes += static_cast<u64>(run.Boxes.Min.capacity() + run.Boxes.Max.capacity()) * sizeof(glm::vec3);
        }
        return bytes;
    }

    f32 GroomGuideDisplacementReach(const GroomStrandSimulation* simulation) noexcept
    {
        f32 reach = 0.0f;
        if (simulation == nullptr)
        {
            return reach;
        }
        for (const glm::vec3& displacement : simulation->Displacements.Displacements)
        {
            const f32 length = glm::length(displacement);
            if (std::isfinite(length))
            {
                reach = std::max(reach, length);
            }
        }
        return reach;
    }

    std::vector<GroomCasterStrandBox> CollectGroomCasterStrandBoxes(std::span<const GroomCasterStrand> strands)
    {
        std::vector<GroomCasterStrandBox> boxes;
        boxes.reserve(strands.size());
        for (const GroomCasterStrand& strand : strands)
        {
            boxes.push_back({ strand.BoundsMin, strand.BoundsMax });
        }
        return boxes;
    }

    GroomCasterPose BuildGroomCasterPose(std::span<const GroomCasterRun> runs, std::span<const u32> strandOrder,
                                         std::span<const GroomCasterStrand> strands)
    {
        GroomCasterPose pose;
        u64 covered = 0;
        for (const GroomCasterRun& run : runs)
        {
            covered += run.Strands;
        }
        // An order that is not these strands' is not trusted: no pose, and the
        // views decide from the rest runs.
        if (runs.empty() || covered != strandOrder.size() || strandOrder.size() != strands.size())
        {
            return pose;
        }
        pose.Runs.reserve(runs.size());
        u32 first = 0;
        for (const GroomCasterRun& run : runs)
        {
            GroomCasterRunPose part;
            part.FirstStrand = first;
            part.StrandCount = run.Strands;
            part.FirstSample = static_cast<u32>(pose.Samples.size());
            for (u32 k = 0; k < run.Strands; ++k)
            {
                const u32 strand = strandOrder[first + k];
                if (strand >= strands.size())
                {
                    return {};
                }
                const GroomCasterStrand& summary = strands[strand];
                if (summary.BoundsMin.x <= summary.BoundsMax.x)
                {
                    part.Reach = std::max(part.Reach, glm::length(summary.BoundsMax - summary.BoundsMin));
                }
                // The FIRST strands of the run's hashed order: a uniform random
                // sample of it, whatever its size.
                if (k < kGroomCasterPoseSamples)
                {
                    pose.Samples.push_back({ summary.Moments, summary.Length, strand });
                    part.SampleLength += summary.Length;
                }
            }
            part.SampleCount = static_cast<u32>(pose.Samples.size()) - part.FirstSample;
            first += run.Strands;
            pose.Runs.push_back(part);
        }
        return pose;
    }

    void SkinGroomCasterPose(GroomCasterPose& pose, std::span<const u32> strandOrder, std::span<const u32> rootCurves,
                             std::span<const GroomCasterStrandBox> strandBoxes, const GroomBindingAsset& binding,
                             const GroomSurfaceView& surface, const GroomSkinningView& skinning, u32 boneCount,
                             u32 baseCurveCount)
    {
        pose.Skinned = false;
        if (!pose.IsUsable() || strandOrder.size() != rootCurves.size() || strandBoxes.size() != rootCurves.size())
        {
            return;
        }
        const bool bindingSpans = binding.GetRootCount() == baseCurveCount;
        const bool influences = skinning.BoneIds != nullptr && skinning.Weights != nullptr && skinning.Stride >= 32u &&
                                skinning.VertexCount >= surface.VertexCount;
        constexpr f32 kBig = std::numeric_limits<f32>::max();
        const auto grow = [](glm::vec3& lo, glm::vec3& hi, const glm::vec3& boxMin, const glm::vec3& boxMax)
        {
            lo = glm::min(lo, boxMin);
            hi = glm::max(hi, boxMax);
        };
        for (GroomCasterRunPose& run : pose.Runs)
        {
            if (static_cast<u64>(run.FirstStrand) + run.StrandCount > strandOrder.size())
            {
                return;
            }
            GroomRootBoneBounds& boxes = run.Boxes;
            boxes = {};
            boxes.Min.assign(boneCount, glm::vec3(kBig));
            boxes.Max.assign(boneCount, glm::vec3(-kBig));
            boxes.RestMin = boxes.HeldMin = glm::vec3(kBig);
            boxes.RestMax = boxes.HeldMax = glm::vec3(-kBig);
            for (u32 k = 0; k < run.StrandCount; ++k)
            {
                const u32 strand = strandOrder[run.FirstStrand + k];
                if (strand >= rootCurves.size())
                {
                    return;
                }
                const GroomCasterStrandBox& box = strandBoxes[strand];
                if (!(box.Min.x <= box.Max.x))
                {
                    continue; // a strand that measured nothing
                }
                const u32 curve = rootCurves[strand];
                const auto hold = [&]()
                {
                    // The kernel holds it at rest: it stays in the groom's space.
                    grow(boxes.HeldMin, boxes.HeldMax, box.Min, box.Max);
                    boxes.HasHeld = true;
                };
                if (!bindingSpans || curve >= baseCurveCount)
                {
                    hold();
                    continue;
                }
                const GroomRootBinding& record = binding.GetRoot(curve);
                if (!surface.TriangleInRange(record.TriangleIndex))
                {
                    hold();
                    continue;
                }
                const glm::uvec3 corners = surface.TriangleIndices(record.TriangleIndex);
                const GroomSurfaceFrame frame = MakeGroomSurfaceFrame(
                    surface.Position(corners.x), surface.Position(corners.y), surface.Position(corners.z),
                    record.Barycentric);
                if (!frame.Valid)
                {
                    hold();
                    continue;
                }
                // The strand's box in the SURFACE's rest space, through its own
                // root's rigid bind map: its rest frame on the surface against
                // its bind frame in the groom.
                const glm::mat3 turn = glm::mat3_cast(frame.Rotation * glm::conjugate(record.RestRotation));
                glm::vec3 lo(kBig);
                glm::vec3 hi(-kBig);
                for (u32 c = 0; c < 8u; ++c)
                {
                    const glm::vec3 p((c & 1u) != 0u ? box.Max.x : box.Min.x, (c & 2u) != 0u ? box.Max.y : box.Min.y,
                                      (c & 4u) != 0u ? box.Max.z : box.Min.z);
                    const glm::vec3 onSurface = frame.Origin + (turn * (p - record.RestOrigin));
                    lo = glm::min(lo, onSurface);
                    hi = glm::max(hi, onSurface);
                }
                bool moved = false;
                if (influences)
                {
                    for (u32 c = 0; c < 3u; ++c)
                    {
                        const auto offset = static_cast<sizet>(corners[static_cast<i32>(c)]) * skinning.Stride;
                        u32 ids[4] = { 0, 0, 0, 0 };
                        f32 weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        std::memcpy(ids, reinterpret_cast<const std::byte*>(skinning.BoneIds) + offset, sizeof(ids));
                        std::memcpy(weights, reinterpret_cast<const std::byte*>(skinning.Weights) + offset,
                                    sizeof(weights));
                        for (u32 w = 0; w < 4u; ++w)
                        {
                            if (!std::isfinite(weights[w]) || weights[w] <= 0.0f || ids[w] >= boneCount)
                            {
                                continue;
                            }
                            grow(boxes.Min[ids[w]], boxes.Max[ids[w]], lo, hi);
                            moved = true;
                        }
                    }
                }
                if (!moved)
                {
                    grow(boxes.RestMin, boxes.RestMax, lo, hi);
                    boxes.HasRest = true;
                }
            }
        }
        pose.Skinned = true;
    }

    std::array<f32, 6> RotateGroomCasterMoments(const std::array<f32, 6>& moments, const glm::mat3& rotation) noexcept
    {
        const glm::mat3 m(moments[0], moments[3], moments[4],  // column 0: xx, xy, xz
                          moments[3], moments[1], moments[5],  // column 1: xy, yy, yz
                          moments[4], moments[5], moments[2]); // column 2: xz, yz, zz
        const glm::mat3 r = rotation * m * glm::transpose(rotation);
        return { r[0][0], r[1][1], r[2][2], r[1][0], r[2][0], r[2][1] };
    }

    namespace
    {
        // The run's posed moments: its rest moments plus the sample's change,
        // scaled from the sample's length to the run's. At the bind pose the
        // change is zero and the moments are exactly the rest ones; elsewhere
        // the sample estimates the change, unbiased, because it is a uniform
        // random share of the run.
        [[nodiscard]] std::array<f32, 6>
        PosedMoments(const GroomCasterRun& run, const GroomCasterRunPose& part, const GroomCasterPose& pose,
                     const std::function<glm::mat3(const GroomCasterPoseSample&)>& rotationOf)
        {
            std::array<f64, 6> change{};
            for (u32 s = 0; s < part.SampleCount; ++s)
            {
                const GroomCasterPoseSample& sample = pose.Samples[part.FirstSample + s];
                const std::array<f32, 6> turned = RotateGroomCasterMoments(sample.Moments, rotationOf(sample));
                for (sizet m = 0; m < 6u; ++m)
                {
                    change[m] += static_cast<f64>(turned[m]) - static_cast<f64>(sample.Moments[m]);
                }
            }
            const f64 scale = part.SampleLength > 0.0f ? static_cast<f64>(run.TotalLength) / part.SampleLength : 0.0;
            std::array<f32, 6> out = run.Moments;
            for (sizet m = 0; m < 6u; ++m)
            {
                const f64 value = static_cast<f64>(run.Moments[m]) + (change[m] * scale);
                out[m] = std::isfinite(value) ? static_cast<f32>(value) : run.Moments[m];
            }
            return out;
        }

        [[nodiscard]] bool SamePose(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose) noexcept
        {
            if (pose.Runs.size() != restRuns.size())
            {
                return false;
            }
            for (sizet r = 0; r < restRuns.size(); ++r)
            {
                if (pose.Runs[r].StrandCount != restRuns[r].Strands)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool PoseGroomCasterRuns(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                             const std::function<glm::mat3(const GroomCasterPoseSample&)>& rotationOf,
                             const std::function<bool(sizet run, glm::vec3& min, glm::vec3& max)>& posedBoxOf,
                             f32 reachShare, const GroomCasterPosePadding& padding,
                             std::vector<GroomCasterRun>& outRuns)
    {
        if (!pose.IsUsable() || !SamePose(restRuns, pose))
        {
            return false;
        }
        outRuns.assign(restRuns.begin(), restRuns.end());
        for (sizet r = 0; r < restRuns.size(); ++r)
        {
            GroomCasterRun& run = outRuns[r];
            const GroomCasterRunPose& part = pose.Runs[r];
            if (!run.MomentsKnown)
            {
                continue; // a run without moments is measured against the caster's box
            }
            run.Moments = PosedMoments(run, part, pose, rotationOf);
            glm::vec3 lo{ 0.0f };
            glm::vec3 hi{ 0.0f };
            if (posedBoxOf(r, lo, hi) && lo.x <= hi.x)
            {
                const f32 pad = (part.Reach * std::max(reachShare, 0.0f)) + std::max(padding.Displacement, 0.0f) +
                                std::max(padding.Radius, 0.0f);
                run.BoundsMin = lo - glm::vec3(pad);
                run.BoundsMax = hi + glm::vec3(pad);
            }
            // No posed root at all: the rest box stays, which is what a coat
            // held at rest draws.
        }
        return true;
    }

    bool PoseGroomCasterRunsBySkeleton(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                       std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                       const GroomDeformationInputs& inputs, const GroomCasterPosePadding& padding,
                                       std::vector<GroomCasterRun>& outRuns)
    {
        if (!pose.Skinned || !inputs.Surface.IsUsable() || !inputs.Skinning.IsSkinned())
        {
            return false;
        }
        const u32 roots = binding.GetRootCount();
        const std::span<const glm::mat4> palette = inputs.Skinning.Palette;
        return PoseGroomCasterRuns(
            restRuns, pose,
            [&](const GroomCasterPoseSample& sample)
            {
                // The sampled root's frame this frame, built from its skinned
                // triangle as EvaluateGroomRootTransforms builds it; a root that
                // cannot be framed is held at rest.
                const u32 curve = sample.Strand < rootCurves.size() ? rootCurves[sample.Strand] : roots;
                if (curve >= roots)
                {
                    return glm::mat3(1.0f);
                }
                const GroomRootBinding& record = binding.GetRoot(curve);
                if (!inputs.Surface.TriangleInRange(record.TriangleIndex))
                {
                    return glm::mat3(1.0f);
                }
                const glm::uvec3 corners = inputs.Surface.TriangleIndices(record.TriangleIndex);
                std::array<glm::vec3, 3> posed{};
                for (u32 c = 0; c < 3u; ++c)
                {
                    const u32 vertex = corners[static_cast<i32>(c)];
                    bool weighted = false;
                    const glm::vec3 skinned =
                        SkinGroomSurfaceVertex(inputs.Skinning, vertex, inputs.Surface.Position(vertex), palette, weighted);
                    posed[c] = glm::vec3(inputs.SurfaceToGroom * glm::vec4(skinned, 1.0f));
                }
                const GroomSurfaceFrame frame = MakeGroomSurfaceFrame(posed[0], posed[1], posed[2], record.Barycentric);
                return frame.Valid ? glm::mat3_cast(frame.Rotation * glm::conjugate(record.RestRotation))
                                   : glm::mat3(1.0f);
            },
            [&](sizet run, glm::vec3& lo, glm::vec3& hi)
            { return PoseGroomRootBoneBounds(pose.Runs[run].Boxes, palette, inputs.SurfaceToGroom, lo, hi); },
            kGroomCasterBlendAllowance, padding, outRuns);
    }

    bool PoseGroomCasterRunsByRoots(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                    std::span<const u32> strandOrder, std::span<const u32> rootCurves,
                                    const GroomBindingAsset& binding, std::span<const GroomRootTransform> transforms,
                                    const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns)
    {
        if (strandOrder.size() != rootCurves.size() ||
            !std::ranges::all_of(strandOrder, [&](u32 strand)
                                 { return strand < rootCurves.size(); }))
        {
            return false;
        }
        const u32 roots = binding.GetRootCount();
        const auto rootOf = [&](u32 strand) -> const GroomRootTransform*
        {
            const u32 curve = rootCurves[strand];
            return curve < transforms.size() && curve < roots && transforms[curve].Valid ? &transforms[curve] : nullptr;
        };
        std::vector<u8> heldAtRest(restRuns.size(), 0u);
        const bool posed = PoseGroomCasterRuns(
            restRuns, pose,
            [&](const GroomCasterPoseSample& sample)
            {
                const GroomRootTransform* transform = sample.Strand < rootCurves.size() ? rootOf(sample.Strand) : nullptr;
                if (transform == nullptr)
                {
                    return glm::mat3(1.0f);
                }
                const GroomRootBinding& record = binding.GetRoot(rootCurves[sample.Strand]);
                return glm::mat3_cast(transform->Rotation * glm::conjugate(record.RestRotation));
            },
            [&](sizet run, glm::vec3& lo, glm::vec3& hi)
            {
                // Every root of the run, posed.
                lo = glm::vec3(std::numeric_limits<f32>::max());
                hi = glm::vec3(std::numeric_limits<f32>::lowest());
                const GroomCasterRunPose& part = pose.Runs[run];
                if (static_cast<u64>(part.FirstStrand) + part.StrandCount > strandOrder.size())
                {
                    return false;
                }
                for (u32 k = 0; k < part.StrandCount; ++k)
                {
                    const GroomRootTransform* transform = rootOf(strandOrder[part.FirstStrand + k]);
                    if (transform == nullptr)
                    {
                        heldAtRest[run] = 1u;
                        continue;
                    }
                    lo = glm::min(lo, transform->Origin);
                    hi = glm::max(hi, transform->Origin);
                }
                return lo.x <= hi.x;
            },
            1.0f, padding, outRuns);
        if (!posed)
        {
            return false;
        }
        // A root at rest leaves its strand at rest, inside the run's rest box.
        for (sizet r = 0; r < outRuns.size(); ++r)
        {
            if (heldAtRest[r] != 0u)
            {
                outRuns[r].BoundsMin = glm::min(outRuns[r].BoundsMin, restRuns[r].BoundsMin);
                outRuns[r].BoundsMax = glm::max(outRuns[r].BoundsMax, restRuns[r].BoundsMax);
            }
        }
        return true;
    }
} // namespace OloEngine
