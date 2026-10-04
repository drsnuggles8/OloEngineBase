#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomCasterPose.h"

#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Task/ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kBig = std::numeric_limits<f32>::max();

        template<typename T>
        [[nodiscard]] u64 VectorBytes(const std::vector<T>& v) noexcept
        {
            return static_cast<u64>(v.capacity()) * sizeof(T);
        }

        [[nodiscard]] bool Finite(const glm::vec3& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        void Grow(glm::vec3& lo, glm::vec3& hi, const glm::vec3& boxMin, const glm::vec3& boxMax) noexcept
        {
            lo = glm::min(lo, boxMin);
            hi = glm::max(hi, boxMax);
        }

        // The farthest a drawn point of an entry sits from its own root: the
        // farthest corner of its offset box.
        [[nodiscard]] f32 Reach(const GroomCasterPoseEntry& entry) noexcept
        {
            const glm::vec3 farthest = glm::max(glm::abs(entry.OffsetMin), glm::abs(entry.OffsetMax));
            return glm::length(farthest);
        }

        // The entry's offset box turned by `rotation`, as a box: the centre
        // turned, the half extents through |R|.
        void TurnedOffsets(const GroomCasterPoseEntry& entry, const glm::mat3& rotation, glm::vec3& outMin,
                           glm::vec3& outMax) noexcept
        {
            const glm::vec3 centre = 0.5f * (entry.OffsetMin + entry.OffsetMax);
            const glm::vec3 half = 0.5f * (entry.OffsetMax - entry.OffsetMin);
            const glm::mat3 absolute(glm::abs(rotation[0]), glm::abs(rotation[1]), glm::abs(rotation[2]));
            const glm::vec3 turnedCentre = rotation * centre;
            const glm::vec3 turnedHalf = absolute * half;
            outMin = turnedCentre - turnedHalf;
            outMax = turnedCentre + turnedHalf;
        }

        void AddMoments(std::array<f64, 6>& sum, const std::array<f32, 6>& moments) noexcept
        {
            for (sizet m = 0; m < 6u; ++m)
            {
                sum[m] += static_cast<f64>(moments[m]);
            }
        }

        [[nodiscard]] bool SamePose(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose) noexcept
        {
            if (pose.Runs.size() != restRuns.size())
            {
                return false;
            }
            for (sizet r = 0; r < restRuns.size(); ++r)
            {
                if (pose.Runs[r].Strands != restRuns[r].Strands)
                {
                    return false;
                }
            }
            return true;
        }

        // One run's posed sums, built from its entries in pieces.
        struct RunSums
        {
            std::array<f64, 6> Moments{};
            f64 Length = 0.0;
            f64 Loss = 0.0;
            glm::vec3 Min{ kBig };
            glm::vec3 Max{ -kBig };
            bool AtRest = false; // some strand may be drawn at rest: the run's rest box joins
        };

        void Merge(RunSums& into, const RunSums& part) noexcept
        {
            for (sizet m = 0; m < 6u; ++m)
            {
                into.Moments[m] += part.Moments[m];
            }
            into.Length += part.Length;
            into.Loss += part.Loss;
            Grow(into.Min, into.Max, part.Min, part.Max);
            into.AtRest = into.AtRest || part.AtRest;
        }

        // Each run's entries in pieces of at most kPiece, so one large run is
        // spread across workers.
        struct Piece
        {
            u32 Run = 0;
            u32 First = 0;
            u32 End = 0;
        };
        constexpr u32 kPiece = 2048;

        [[nodiscard]] std::vector<Piece> Pieces(const GroomCasterPose& pose)
        {
            std::vector<Piece> pieces;
            for (u32 r = 0; r < static_cast<u32>(pose.Runs.size()); ++r)
            {
                const GroomCasterRunPose& run = pose.Runs[r];
                for (u32 first = run.FirstEntry; first < run.FirstEntry + run.EntryCount; first += kPiece)
                {
                    pieces.push_back({ r, first, std::min(first + kPiece, run.FirstEntry + run.EntryCount) });
                }
            }
            return pieces;
        }

        // The pieces' sums into each run, the held strands and the padding:
        // the posed runs. A run without moments keeps its rest numbers.
        void FinishRuns(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                        std::span<const Piece> pieces, std::span<const RunSums> sums,
                        const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns)
        {
            std::vector<RunSums> runs(pose.Runs.size());
            for (sizet p = 0; p < pieces.size(); ++p)
            {
                Merge(runs[pieces[p].Run], sums[p]);
            }
            outRuns.assign(restRuns.begin(), restRuns.end());
            for (sizet r = 0; r < restRuns.size(); ++r)
            {
                GroomCasterRun& out = outRuns[r];
                if (!out.MomentsKnown)
                {
                    continue; // measured against the caster's box, as at rest
                }
                const GroomCasterRunPose& part = pose.Runs[r];
                RunSums& sum = runs[r];
                if (part.HasHeld)
                {
                    AddMoments(sum.Moments, part.HeldMoments);
                    sum.Length += part.HeldLength;
                    Grow(sum.Min, sum.Max, part.HeldMin, part.HeldMax);
                }
                f32 displacement = 0.0f;
                if (pose.SlotWeightsKnown && padding.SlotsComplete)
                {
                    // Each strand's weighted sum of its slots' variation, summed
                    // over the run: the slots' summed weights against their
                    // variation. No slot: the run is not simulated.
                    for (u32 k = 0; k < part.SlotWeightCount; ++k)
                    {
                        const GroomCasterSlotWeight& w = pose.SlotWeights[part.FirstSlotWeight + k];
                        if (w.Slot < padding.SlotVariation.size())
                        {
                            sum.Loss += static_cast<f64>(w.Weight) * padding.SlotVariation[w.Slot];
                            displacement = std::max(displacement, padding.SlotDisplacement[w.Slot]);
                        }
                    }
                }
                else
                {
                    f32 variation = 0.0f;
                    for (u32 role = 0; role < GroomCoatRoleCount; ++role)
                    {
                        if ((part.RoleMask & (1u << role)) != 0u)
                        {
                            displacement = std::max(displacement, padding.Displacement[role]);
                            variation = std::max(variation, padding.Variation[role]);
                        }
                    }
                    sum.Loss += static_cast<f64>(variation) * static_cast<f64>(part.Strands);
                }
                for (sizet m = 0; m < 6u; ++m)
                {
                    out.Moments[m] = static_cast<f32>(sum.Moments[m]);
                }
                out.TotalLength = static_cast<f32>(sum.Length);
                out.ProjectedLengthLoss = static_cast<f32>(sum.Loss);
                if (sum.AtRest)
                {
                    Grow(sum.Min, sum.Max, restRuns[r].BoundsMin, restRuns[r].BoundsMax);
                }
                if (sum.Min.x <= sum.Max.x)
                {
                    const f32 pad = std::max(displacement, 0.0f) + std::max(padding.Radius, 0.0f);
                    out.BoundsMin = sum.Min - glm::vec3(pad);
                    out.BoundsMax = sum.Max + glm::vec3(pad);
                }
            }
        }
    } // namespace

    u64 GroomCasterPose::CpuBytes() const noexcept
    {
        return VectorBytes(Runs) + VectorBytes(Entries) + VectorBytes(Triangles) + VectorBytes(EntryCurves) +
               VectorBytes(SlotWeights);
    }

    u64 GroomCasterPoseSurface::CpuBytes() const noexcept
    {
        return VectorBytes(Vertices) + VectorBytes(Corners);
    }

    std::vector<GroomCasterLocalBox> CollectGroomCasterLocalBoxes(std::span<const GroomStrandVertex> restVertices,
                                                                  std::span<const u32> indices,
                                                                  std::span<const u32> strandFirstIndex)
    {
        std::vector<GroomCasterLocalBox> boxes(strandFirstIndex.size(), GroomCasterLocalBox{ glm::vec3(kBig), glm::vec3(-kBig) });
        for (sizet s = 0; s < strandFirstIndex.size(); ++s)
        {
            const sizet end = s + 1u < strandFirstIndex.size() ? strandFirstIndex[s + 1u] : indices.size();
            if (strandFirstIndex[s] > end || end > indices.size())
            {
                return {};
            }
            for (sizet i = strandFirstIndex[s]; i < end; ++i)
            {
                if (indices[i] >= restVertices.size())
                {
                    return {};
                }
                const GroomStrandVertex& v = restVertices[indices[i]];
                const glm::vec3 radius(std::max(v.Radius, 0.0f));
                Grow(boxes[s].Min, boxes[s].Max, v.Position - radius, v.Position + radius);
            }
        }
        return boxes;
    }

    GroomCasterPose BuildGroomCasterPose(std::span<const GroomCasterRun> runs, std::span<const u32> strandOrder,
                                         std::span<const GroomCasterStrand> strands, std::span<const u32> strandCurves,
                                         const GroomBindingAsset& binding, std::span<const GroomCasterLocalBox> localBoxes,
                                         const GroomGuideInfluenceTable* influence)
    {
        const bool exactBoxes = localBoxes.size() == strands.size();
        const std::span<const GroomGuideWeights> weights = influence != nullptr
                                                               ? std::span<const GroomGuideWeights>(influence->GetWeights())
                                                               : std::span<const GroomGuideWeights>{};
        std::vector<f64> slotSum(influence != nullptr ? influence->GetGuideCount() : 0u, 0.0);
        std::vector<u32> slotsTouched;
        GroomCasterPose pose;
        u64 covered = 0;
        for (const GroomCasterRun& run : runs)
        {
            covered += run.Strands;
        }
        // An order or a table that is not these strands' is not trusted: no
        // pose, and the views decide from the rest runs.
        if (runs.empty() || covered != strandOrder.size() || strandOrder.size() != strands.size() ||
            strandCurves.size() != strands.size())
        {
            return pose;
        }
        const u32 roots = binding.GetRootCount();
        std::vector<std::pair<u32, u32>> keyed; // (triangle, strand), one run at a time
        std::vector<u32> slots;                 // triangle -> slot, grown as triangles appear
        constexpr u32 kNoSlot = ~0u;
        pose.Runs.reserve(runs.size());
        u32 first = 0;
        for (const GroomCasterRun& run : runs)
        {
            GroomCasterRunPose part;
            part.FirstEntry = static_cast<u32>(pose.Entries.size());
            part.Strands = run.Strands;
            part.HeldMin = glm::vec3(kBig);
            part.HeldMax = glm::vec3(-kBig);
            keyed.clear();
            for (u32 k = 0; k < run.Strands; ++k)
            {
                const u32 strand = strandOrder[first + k];
                if (strand >= strands.size())
                {
                    return {};
                }
                const GroomCasterStrand& summary = strands[strand];
                if (summary.Role < GroomCoatRoleCount)
                {
                    part.RoleMask |= 1u << summary.Role;
                }
                else
                {
                    part.RoleMask = (1u << GroomCoatRoleCount) - 1u;
                }
                const u32 curve = strandCurves[strand];
                if (curve < weights.size())
                {
                    // The blend divides by the weight it applies, so each
                    // strand's weights count normalised to one.
                    const GroomGuideWeights& blend = weights[curve];
                    f64 applied = 0.0;
                    for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
                    {
                        if (blend.Guides[k] < slotSum.size() && blend.Weights[k] > 0.0f)
                        {
                            applied += blend.Weights[k];
                        }
                    }
                    for (u32 k = 0; k < GroomGuideInfluenceCount && applied > 0.0; ++k)
                    {
                        const u32 slot = blend.Guides[k];
                        if (slot < slotSum.size() && blend.Weights[k] > 0.0f)
                        {
                            if (!(slotSum[slot] > 0.0))
                            {
                                slotsTouched.push_back(slot);
                            }
                            slotSum[slot] += static_cast<f64>(blend.Weights[k]) / applied;
                        }
                    }
                }
                if (curve < roots)
                {
                    keyed.emplace_back(binding.GetRoot(curve).TriangleIndex, strand);
                    continue;
                }
                // No binding record: drawn at rest in every pose.
                for (sizet m = 0; m < 6u; ++m)
                {
                    part.HeldMoments[m] += summary.Moments[m];
                }
                part.HeldLength += summary.Length;
                if (summary.BoundsMin.x <= summary.BoundsMax.x)
                {
                    Grow(part.HeldMin, part.HeldMax, summary.BoundsMin, summary.BoundsMax);
                }
                part.HasHeld = true;
            }
            std::ranges::sort(keyed);
            for (sizet i = 0; i < keyed.size();)
            {
                const u32 triangle = keyed[i].first;
                if (triangle >= slots.size())
                {
                    slots.resize(static_cast<sizet>(triangle) + 1u, kNoSlot);
                }
                if (slots[triangle] == kNoSlot)
                {
                    slots[triangle] = static_cast<u32>(pose.Triangles.size());
                    pose.Triangles.push_back(triangle);
                }
                // One entry per triangle AND bind frame: the binder frames every
                // root of a triangle alike, and a record that does not is kept
                // apart rather than turned by another's rotation.
                GroomCasterPoseEntry entry;
                entry.Slot = slots[triangle];
                entry.FirstCurve = static_cast<u32>(pose.EntryCurves.size());
                const glm::quat bind = binding.GetRoot(strandCurves[keyed[i].second]).RestRotation;
                const glm::mat3 intoBind = glm::mat3_cast(glm::conjugate(bind));
                entry.OffsetMin = glm::vec3(kBig);
                entry.OffsetMax = glm::vec3(-kBig);
                std::array<f64, 6> moments{};
                f64 length = 0.0;
                for (; i < keyed.size() && keyed[i].first == triangle; ++i)
                {
                    const u32 strand = keyed[i].second;
                    const GroomRootBinding& record = binding.GetRoot(strandCurves[strand]);
                    if (std::abs(glm::dot(record.RestRotation, bind)) < 1.0f - 1.0e-6f)
                    {
                        break; // a different bind frame on the same triangle: its own entry
                    }
                    const GroomCasterStrand& summary = strands[strand];
                    AddMoments(moments, summary.Moments);
                    length += summary.Length;
                    if (exactBoxes)
                    {
                        if (localBoxes[strand].Min.x <= localBoxes[strand].Max.x)
                        {
                            Grow(entry.OffsetMin, entry.OffsetMax, localBoxes[strand].Min, localBoxes[strand].Max);
                        }
                    }
                    else if (summary.BoundsMin.x <= summary.BoundsMax.x)
                    {
                        // The strand's box less its origin, each corner into the
                        // bind frame.
                        for (u32 c = 0; c < 8u; ++c)
                        {
                            const glm::vec3 p{ (c & 1u) != 0u ? summary.BoundsMax.x : summary.BoundsMin.x,
                                               (c & 2u) != 0u ? summary.BoundsMax.y : summary.BoundsMin.y,
                                               (c & 4u) != 0u ? summary.BoundsMax.z : summary.BoundsMin.z };
                            const glm::vec3 local = intoBind * (p - record.RestOrigin);
                            Grow(entry.OffsetMin, entry.OffsetMax, local, local);
                        }
                    }
                    pose.EntryCurves.push_back(strandCurves[strand]);
                    ++entry.Strands;
                }
                std::array<f32, 6> groomSpace{};
                for (sizet m = 0; m < 6u; ++m)
                {
                    groomSpace[m] = static_cast<f32>(moments[m]);
                }
                entry.Moments = RotateGroomCasterMoments(groomSpace, intoBind);
                entry.Length = static_cast<f32>(length);
                if (!(entry.OffsetMin.x <= entry.OffsetMax.x))
                {
                    entry.OffsetMin = entry.OffsetMax = glm::vec3(0.0f); // strands that drew nothing
                }
                pose.Entries.push_back(entry);
            }
            part.EntryCount = static_cast<u32>(pose.Entries.size()) - part.FirstEntry;
            part.FirstSlotWeight = static_cast<u32>(pose.SlotWeights.size());
            std::ranges::sort(slotsTouched);
            for (const u32 slot : slotsTouched)
            {
                pose.SlotWeights.push_back({ slot, static_cast<f32>(slotSum[slot]) });
                slotSum[slot] = 0.0;
            }
            part.SlotWeightCount = static_cast<u32>(slotsTouched.size());
            slotsTouched.clear();
            first += run.Strands;
            pose.Runs.push_back(part);
        }
        pose.SlotWeightsKnown = influence != nullptr;
        return pose;
    }

    GroomCasterPoseSurface BuildGroomCasterPoseSurface(const GroomCasterPose& pose, const GroomSurfaceView& surface)
    {
        GroomCasterPoseSurface out;
        if (!pose.IsUsable() || !surface.IsUsable())
        {
            return out;
        }
        constexpr u32 kUnused = ~0u;
        std::vector<u32> compact(surface.VertexCount, kUnused);
        out.Corners.reserve(pose.Triangles.size());
        for (const u32 triangle : pose.Triangles)
        {
            if (!surface.TriangleInRange(triangle))
            {
                out.Corners.emplace_back(GroomCasterPoseSurface::kNoCorners);
                continue;
            }
            const glm::uvec3 corners = surface.TriangleIndices(triangle);
            glm::uvec3 local{ 0u };
            for (i32 c = 0; c < 3; ++c)
            {
                u32& slot = compact[corners[c]];
                if (slot == kUnused)
                {
                    slot = static_cast<u32>(out.Vertices.size());
                    out.Vertices.push_back(corners[c]);
                }
                local[c] = slot;
            }
            out.Corners.push_back(local);
        }
        return out;
    }

    std::array<f32, 6> RotateGroomCasterMoments(const std::array<f32, 6>& moments, const glm::mat3& rotation) noexcept
    {
        const glm::mat3 m(moments[0], moments[3], moments[4],  // column 0: xx, xy, xz
                          moments[3], moments[1], moments[5],  // column 1: xy, yy, yz
                          moments[4], moments[5], moments[2]); // column 2: xz, yz, zz
        const glm::mat3 r = rotation * m * glm::transpose(rotation);
        return { r[0][0], r[1][1], r[2][2], r[1][0], r[2][0], r[2][1] };
    }

    GroomCasterPosePadding MeasureGroomCasterPosePadding(const GroomStrandSimulation* simulation,
                                                         std::span<const u16> curveGroups, const GroomCoatContext* coat,
                                                         f32 radius)
    {
        GroomCasterPosePadding padding;
        padding.Radius = std::isfinite(radius) ? std::max(radius, 0.0f) : 0.0f;
        if (simulation == nullptr || !simulation->Displacements.IsUsable())
        {
            return padding;
        }
        const GroomGuideDisplacements& d = simulation->Displacements;
        const std::span<const u32> guideCurves = simulation->Influence != nullptr
                                                     ? std::span<const u32>(simulation->Influence->GetGuideCurves())
                                                     : std::span<const u32>{};
        const bool roles = coat != nullptr && coat->IsActive();
        // Per slot, what the strands blend: complete when every slot has a
        // displacement this frame (no strand's weights are renormalised).
        const sizet slots = simulation->GuideOfSlot.size();
        padding.SlotDisplacement.assign(slots, 0.0f);
        padding.SlotVariation.assign(slots, 0.0f);
        padding.SlotsComplete = slots > 0u;
        std::vector<f32> guideDisplacement(d.GuideCount(), 0.0f);
        std::vector<f32> guideVariation(d.GuideCount(), 0.0f);
        for (u32 g = 0; g < d.GuideCount(); ++g)
        {
            f32 farthest = 0.0f;
            f32 variation = 0.0f;
            for (u32 p = d.GuideOffsets[g]; p < d.GuideOffsets[g + 1u]; ++p)
            {
                const f32 length = glm::length(d.Displacements[p]);
                if (std::isfinite(length))
                {
                    farthest = std::max(farthest, length);
                }
                if (p + 1u < d.GuideOffsets[g + 1u])
                {
                    const f32 step = glm::length(d.Displacements[p + 1u] - d.Displacements[p]);
                    if (std::isfinite(step))
                    {
                        variation += step;
                    }
                }
            }
            // The guide's coat role, as CoatOfCurve resolves its curve's. A
            // guide whose curve cannot be named counts for every role.
            u32 role = static_cast<u32>(GroomCoatRole::Unassigned);
            bool known = !roles;
            const u32 slot = d.SlotOfGuide[g];
            if (roles && slot < guideCurves.size() && guideCurves[slot] < curveGroups.size())
            {
                role = static_cast<u32>(coat->GroupDesc(curveGroups[guideCurves[slot]]).GetRole());
                known = role < GroomCoatRoleCount;
            }
            for (u32 r = 0; r < GroomCoatRoleCount; ++r)
            {
                if (!known || r == role)
                {
                    padding.Displacement[r] = std::max(padding.Displacement[r], farthest);
                    padding.Variation[r] = std::max(padding.Variation[r], variation);
                }
            }
            guideDisplacement[g] = farthest;
            guideVariation[g] = variation;
        }
        for (sizet s = 0; s < slots; ++s)
        {
            const u32 g = simulation->GuideOfSlot[s];
            if (g >= d.GuideCount() || d.GuideOffsets[g + 1u] <= d.GuideOffsets[g])
            {
                // Left out with no stand-in, or a guide with no points: the
                // blend skips it and renormalises its strands' weights.
                padding.SlotsComplete = false;
                continue;
            }
            padding.SlotDisplacement[s] = guideDisplacement[g];
            padding.SlotVariation[s] = guideVariation[g];
        }
        return padding;
    }

    f32 GroomCasterFrameTolerance(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                  f32 positionTolerance) noexcept
    {
        constexpr f32 kInfinite = std::numeric_limits<f32>::infinity();
        const glm::vec3 edge1 = v1 - v0;
        const glm::vec3 edge2 = v2 - v0;
        const f32 area = glm::length(glm::cross(edge1, edge2));
        const f32 first = glm::length(edge1);
        if (!std::isfinite(area) || !(area >= 1.0e-12f) || !(first > 0.0f))
        {
            return kInfinite;
        }
        // Each corner may sit `epsilon` from where the GPU puts it, so each edge
        // `2 epsilon` from its own. The normal, cross(e1, e2) / |...|, turns by
        // at most |delta cross| / |cross| <= 2 epsilon (|e1| + |e2|) / |cross| to
        // first order; the tangent, e1 less its normal part (e1 is already
        // perpendicular to the normal), by 2 epsilon / |e1| more. The frame's
        // rotation error is at most the sum of its columns' -- doubled, so the
        // second-order terms dropped above stay inside it while the first
        // order is small, and a triangle where it is not is reported as such.
        const f32 epsilon = positionTolerance * (1.0f + std::max({ glm::length(v0), glm::length(v1), glm::length(v2) }));
        const f32 normal = 2.0f * epsilon * (first + glm::length(edge2)) / area;
        if (!(normal < 0.25f))
        {
            return kInfinite;
        }
        const f32 tangent = normal + (2.0f * epsilon / first);
        return 2.0f * (normal + tangent);
    }

    bool PoseGroomCasterRunsBySurface(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                      const GroomCasterPoseSurface& surface, const GroomDeformationInputs& inputs,
                                      const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns)
    {
        if (!pose.IsUsable() || !SamePose(restRuns, pose) || surface.Corners.size() != pose.Triangles.size() ||
            !inputs.Surface.IsUsable())
        {
            return false;
        }
        const bool skinned = inputs.Skinning.IsSkinned();
        const std::span<const glm::mat4> palette = inputs.Skinning.Palette;

        // THE SURFACE, once per vertex: skinned (or as it stands), into the
        // groom's space -- EvaluateGroomRootTransforms' corners, which
        // GroomRootFrames.comp reproduces from the same inputs. Scratch,
        // reused across frames; thread_local so two callers never share it.
        thread_local std::vector<glm::vec3> s_Corners;
        std::vector<glm::vec3>& corners = s_Corners;
        corners.resize(surface.Vertices.size());
        ParallelFor("GroomCasterPoseSurface", static_cast<i32>(surface.Vertices.size()), 2048, [&](i32 index)
                    {
                        const u32 v = surface.Vertices[static_cast<sizet>(index)];
                        glm::vec3 p{ std::numeric_limits<f32>::quiet_NaN() };
                        if (v < inputs.Surface.VertexCount)
                        {
                            p = inputs.Surface.Position(v);
                            if (skinned)
                            {
                                bool weighted = false;
                                p = SkinGroomSurfaceVertex(inputs.Skinning, v, p, palette, weighted);
                            }
                            p = glm::vec3(inputs.SurfaceToGroom * glm::vec4(p, 1.0f));
                        }
                        corners[static_cast<sizet>(index)] = p; }, EParallelForFlags::BackgroundPriority);

        // EACH TRIANGLE'S FRAME, and how far the GPU's can differ from it.
        enum class Kind : u8
        {
            Framed,    // turned by Rotation, uncertain by Error
            Unknown,   // a frame too ill-conditioned to trust, or none: no credit, full reach, maybe at rest
            NotOnBody, // the surface has no such triangle: held at rest by every evaluation
        };
        struct Frame
        {
            glm::mat3 Rotation{ 1.0f }; // the triangle's frame: tangent, bitangent, normal
            glm::vec3 HullMin{ 0.0f };
            glm::vec3 HullMax{ 0.0f };
            f32 Error = 0.0f;
            f32 Slack = 0.0f;
            Kind Is = Kind::NotOnBody;
        };
        thread_local std::vector<Frame> s_Frames;
        std::vector<Frame>& frames = s_Frames;
        frames.resize(pose.Triangles.size()); // every field a reader uses is written below
        ParallelFor("GroomCasterPoseFrames", static_cast<i32>(pose.Triangles.size()), 1024, [&](i32 index)
                    {
                        Frame& frame = frames[static_cast<sizet>(index)];
                        frame.Is = Kind::NotOnBody;
                        frame.Error = 0.0f;
                        const glm::uvec3 c = surface.Corners[static_cast<sizet>(index)];
                        if (c.x == GroomCasterPoseSurface::kNoCorners)
                        {
                            return;
                        }
                        const glm::vec3 p0 = corners[c.x];
                        const glm::vec3 p1 = corners[c.y];
                        const glm::vec3 p2 = corners[c.z];
                        frame.HullMin = glm::min(p0, glm::min(p1, p2));
                        frame.HullMax = glm::max(p0, glm::max(p1, p2));
                        frame.Is = Kind::Unknown;
                        if (!Finite(frame.HullMin) || !Finite(frame.HullMax))
                        {
                            frame.Is = Kind::NotOnBody; // the kernel cannot read it either
                            return;
                        }
                        // The farthest corner from the origin, bounded by the hull.
                        const f32 radius = glm::length(glm::max(glm::abs(frame.HullMin), glm::abs(frame.HullMax)));
                        frame.Slack = kGroomCasterPositionTolerance * (1.0f + radius);
                        // MakeGroomSurfaceFrame's basis -- the normal, the first edge
                        // without its normal part, their cross -- as a matrix, with
                        // GroomCasterFrameTolerance's error from the same lengths.
                        const glm::vec3 edge1 = p1 - p0;
                        const glm::vec3 edge2 = p2 - p0;
                        const glm::vec3 cross = glm::cross(edge1, edge2);
                        const f32 area = glm::length(cross);
                        const f32 first = glm::length(edge1);
                        if (!std::isfinite(area) || !(area >= 1.0e-12f) || !(first > 0.0f))
                        {
                            return;
                        }
                        const glm::vec3 normal = cross / area;
                        const glm::vec3 tangentRaw = edge1 - (normal * glm::dot(normal, edge1));
                        const f32 tangentLength = glm::length(tangentRaw);
                        if (!(tangentLength >= 1.0e-12f))
                        {
                            return;
                        }
                        const f32 epsilon = frame.Slack;
                        const f32 normalError = 2.0f * epsilon * (first + glm::length(edge2)) / area;
                        if (!(normalError < 0.25f))
                        {
                            return;
                        }
                        // The matrix and the GPU's quaternion are one rotation to
                        // f32 rounding: 1e-6 rad covers it.
                        const f32 error = (2.0f * (normalError + normalError + (2.0f * epsilon / first))) + 1.0e-6f;
                        if (error > kGroomCasterMaxFrameError)
                        {
                            return;
                        }
                        const glm::vec3 tangent = tangentRaw / tangentLength;
                        frame.Rotation = glm::mat3(tangent, glm::cross(normal, tangent), normal);
                        frame.Error = error;
                        frame.Is = Kind::Framed; }, EParallelForFlags::BackgroundPriority);

        // EACH ENTRY, turned by its triangle's frame, in pieces.
        const std::vector<Piece> pieces = Pieces(pose);
        thread_local std::vector<RunSums> s_Sums;
        std::vector<RunSums>& sums = s_Sums;
        sums.assign(pieces.size(), RunSums{});
        ParallelFor("GroomCasterPoseEntries", static_cast<i32>(pieces.size()), 1, [&](i32 index)
                    {
                        const Piece& piece = pieces[static_cast<sizet>(index)];
                        RunSums& sum = sums[static_cast<sizet>(index)];
                        for (u32 e = piece.First; e < piece.End; ++e)
                        {
                            const GroomCasterPoseEntry& entry = pose.Entries[e];
                            const Frame& frame = frames[entry.Slot];
                            sum.Length += entry.Length;
                            if (frame.Is == Kind::Framed)
                            {
                                const glm::mat3& turn = frame.Rotation;
                                AddMoments(sum.Moments, RotateGroomCasterMoments(entry.Moments, turn));
                                // Each segment's angle to any direction is uncertain
                                // by the frame's error, its projected length by that
                                // times its length.
                                sum.Loss += static_cast<f64>(frame.Error) * entry.Length;
                                glm::vec3 lo{ 0.0f };
                                glm::vec3 hi{ 0.0f };
                                TurnedOffsets(entry, turn, lo, hi);
                                const glm::vec3 slack(frame.Slack + (frame.Error * Reach(entry)));
                                Grow(sum.Min, sum.Max, frame.HullMin + lo - slack, frame.HullMax + hi + slack);
                                continue;
                            }
                            // Unframed, or not on the body: drawn at rest, or framed
                            // by the GPU some way round. No credit for its length
                            // (its moments stay out, its length is the loss), the
                            // rest box, and every direction its strands could point.
                            sum.Loss += entry.Length;
                            sum.AtRest = true;
                            if (frame.Is == Kind::Unknown)
                            {
                                const glm::vec3 reach(Reach(entry) + frame.Slack);
                                Grow(sum.Min, sum.Max, frame.HullMin - reach, frame.HullMax + reach);
                            }
                        } }, EParallelForFlags::BackgroundPriority);
        FinishRuns(restRuns, pose, pieces, sums, padding, outRuns);
        return true;
    }

    bool PoseGroomCasterRunsByRoots(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                    std::span<const GroomRootTransform> transforms,
                                    const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns)
    {
        if (!pose.IsUsable() || !SamePose(restRuns, pose))
        {
            return false;
        }
        const auto rootOf = [&](u32 curve) -> const GroomRootTransform*
        {
            return curve < transforms.size() && transforms[curve].Valid ? &transforms[curve] : nullptr;
        };
        const std::vector<Piece> pieces = Pieces(pose);
        thread_local std::vector<RunSums> s_Sums;
        std::vector<RunSums>& sums = s_Sums;
        sums.assign(pieces.size(), RunSums{});
        ParallelFor("GroomCasterPoseRoots", static_cast<i32>(pieces.size()), 1, [&](i32 index)
                    {
                        const Piece& piece = pieces[static_cast<sizet>(index)];
                        RunSums& sum = sums[static_cast<sizet>(index)];
                        for (u32 e = piece.First; e < piece.End; ++e)
                        {
                            const GroomCasterPoseEntry& entry = pose.Entries[e];
                            sum.Length += entry.Length;
                            // The strands' posed origins, and the rotation they
                            // share: every root of one triangle frames alike.
                            const GroomRootTransform* turnedBy = nullptr;
                            glm::vec3 lo{ kBig };
                            glm::vec3 hi{ -kBig };
                            u32 atRest = 0;
                            for (u32 k = 0; k < entry.Strands; ++k)
                            {
                                const GroomRootTransform* transform = rootOf(pose.EntryCurves[entry.FirstCurve + k]);
                                if (transform == nullptr)
                                {
                                    ++atRest; // that strand is drawn at rest
                                    continue;
                                }
                                turnedBy = turnedBy != nullptr ? turnedBy : transform;
                                lo = glm::min(lo, transform->Origin);
                                hi = glm::max(hi, transform->Origin);
                            }
                            // The root's frame this frame turns the bind frame's data.
                            const glm::mat3 turn = turnedBy != nullptr ? glm::mat3_cast(turnedBy->Rotation) : glm::mat3(1.0f);
                            if (atRest > 0u)
                            {
                                // Some strand is drawn at rest: no credit for the
                                // entry's length, and the rest box joins.
                                sum.Loss += entry.Length;
                                sum.AtRest = true;
                            }
                            else
                            {
                                AddMoments(sum.Moments, RotateGroomCasterMoments(entry.Moments, turn));
                            }
                            if (turnedBy == nullptr)
                            {
                                continue;
                            }
                            glm::vec3 offsetMin{ 0.0f };
                            glm::vec3 offsetMax{ 0.0f };
                            TurnedOffsets(entry, turn, offsetMin, offsetMax);
                            Grow(sum.Min, sum.Max, lo + offsetMin, hi + offsetMax);
                        } }, EParallelForFlags::BackgroundPriority);
        FinishRuns(restRuns, pose, pieces, sums, padding, outRuns);
        return true;
    }
} // namespace OloEngine
