#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomDeformation.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Task/ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace OloEngine
{
    std::string_view ToString(GroomHistoryResetCause cause)
    {
        switch (cause)
        {
            case GroomHistoryResetCause::None:
                return "None";
            case GroomHistoryResetCause::FirstUse:
                return "FirstUse";
            case GroomHistoryResetCause::TargetChanged:
                return "TargetChanged";
            case GroomHistoryResetCause::TargetTopologyChanged:
                return "TargetTopologyChanged";
            case GroomHistoryResetCause::AnimationReset:
                return "AnimationReset";
            case GroomHistoryResetCause::DeformationSkipped:
                return "DeformationSkipped";
            case GroomHistoryResetCause::Teleport:
                return "Teleport";
            case GroomHistoryResetCause::MorphSurfaceChanged:
                return "MorphSurfaceChanged";
            case GroomHistoryResetCause::BindingChanged:
                return "BindingChanged";
            case GroomHistoryResetCause::SceneTransition:
                return "SceneTransition";
            case GroomHistoryResetCause::Manual:
                return "Manual";
            case GroomHistoryResetCause::Count:
                break;
        }
        return "None";
    }

    glm::vec3 SkinGroomSurfaceVertex(const GroomSkinningView& skinning, u32 vertexIndex,
                                     const glm::vec3& restPosition, std::span<const glm::mat4> palette,
                                     bool& outWeighted) noexcept
    {
        outWeighted = false;
        // `Stride < 32`, not `Stride == 0`: the two memcpys below each read 16
        // bytes at that stride, so anything under 32 overlaps the next vertex's
        // ids with this one's weights. GroomSkinningView::IsSkinned already
        // states 32 as the floor; a public entry point whose precondition is
        // weaker than its own view's is a gap that only shows up as a coat
        // skinned by the wrong four floats.
        if (palette.empty() || skinning.BoneIds == nullptr || skinning.Weights == nullptr ||
            vertexIndex >= skinning.VertexCount || skinning.Stride < 32u)
        {
            return restPosition;
        }

        // The ids and the weights live in one BoneInfluence struct, so both
        // reads are at the same stride from their own base pointer. memcpy
        // rather than a reinterpret_cast through a u32*: the caller's array is
        // BoneInfluence, not u32[4], and reading it as one is a strict-aliasing
        // violation that this repo's clang-cl build is entitled to act on.
        const auto offset = static_cast<sizet>(vertexIndex) * skinning.Stride;
        u32 ids[4] = { 0, 0, 0, 0 };
        f32 weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        std::memcpy(ids, reinterpret_cast<const std::byte*>(skinning.BoneIds) + offset, sizeof(ids));
        std::memcpy(weights, reinterpret_cast<const std::byte*>(skinning.Weights) + offset, sizeof(weights));

        glm::vec3 skinned{ 0.0f };
        f32 weightSum = 0.0f;
        for (i32 influence = 0; influence < 4; ++influence)
        {
            const f32 weight = weights[influence];
            // A non-finite weight is dropped rather than propagated: one NaN in
            // an authored influence would send the whole strand to infinity and
            // poison the groom's bounds, which is a silent failure two
            // subsystems away from its cause.
            if (!std::isfinite(weight) || weight <= 0.0f)
            {
                continue;
            }
            const u32 bone = ids[influence];
            if (bone >= palette.size())
            {
                continue;
            }
            skinned += weight * glm::vec3(palette[bone] * glm::vec4(restPosition, 1.0f));
            weightSum += weight;
        }

        if (weightSum <= 0.0f)
        {
            // An unweighted vertex. Returning the rest position is what the GPU
            // path does with the same input; collapsing it to the origin — which
            // is what dividing by a zero sum would give — would drag every
            // strand rooted there to the model's pivot.
            return restPosition;
        }

        outWeighted = true;
        // Normalised rather than trusted: an authored influence set that sums to
        // 0.97 would shrink the surface by 3%, which reads as a coat floating
        // above the body.
        return skinned / weightSum;
    }

    GroomDeformationStats EvaluateGroomRootTransforms(const GroomAsset& groom, const GroomBindingAsset& binding,
                                                      const GroomDeformationInputs& inputs,
                                                      std::optional<std::span<const u32>> selectedCurves,
                                                      TArray<GroomRootTransform>& outTransforms, bool onlySelectedDefined)
    {
        GroomDeformationStats stats;

        const u32 curveCount = groom.GetCurveCount();
        // Fully written, never partially: an index into this array must be safe
        // for every curve whether or not it was selected — see the header.
        // Cleared into the allocation it already has, in parallel (#1533 E1):
        // on the showcase dog that is ~300k records every frame, and one thread
        // adding them one at a time cost more than evaluating the roots it kept.
        outTransforms.SetNumUninitialized(static_cast<i32>(curveCount), EAllowShrinking::No);
        if (onlySelectedDefined && selectedCurves.has_value())
        {
            GroomRootTransform* const records = outTransforms.GetData();
            for (const u32 curve : *selectedCurves)
                if (curve < curveCount)
                    records[curve] = GroomRootTransform{};
        }
        else
        {
            static constexpr u32 kClearChunk = 16384;
            GroomRootTransform* const records = outTransforms.GetData();
            ParallelFor("GroomRootTransformsClear", static_cast<i32>((curveCount + kClearChunk - 1u) / kClearChunk), 1,
                        [records, curveCount](i32 chunk)
                        {
                            const u32 begin = static_cast<u32>(chunk) * kClearChunk;
                            const u32 end = std::min(begin + kClearChunk, curveCount);
                            std::fill(records + begin, records + end, GroomRootTransform{});
                        });
        }

        if (binding.GetRootCount() != curveCount || !inputs.Surface.IsUsable())
        {
            // The caller is expected to have run GroomBindingAsset::
            // CheckCompatibility and reported the reason; reaching here means it
            // did not, and the honest answer is a coat at rest rather than one
            // deformed by whatever happens to be in memory.
            return stats;
        }

        const bool skinned = inputs.Skinning.IsSkinned();
        // Hoisted: one matrix for every root, and the identity case still pays
        // three multiplies rather than a branch per corner.
        const glm::mat4& surfaceToGroom = inputs.SurfaceToGroom;
        // Previous positions come from the previous POSE, and they are only
        // meaningful if there is one. Both gates matter: the caller's own
        // decision (a teleport it detected) and the skeleton's
        // (HasBoneHistory), because either alone leaves one discontinuity
        // uncovered.
        const bool usePrevPose = inputs.HasHistory && skinned && inputs.Skinning.HasPreviousPose &&
                                 !inputs.Skinning.PrevPalette.empty();
        const glm::mat4& prevSurfaceToGroom =
            (usePrevPose && inputs.PrevSurfaceToGroom.has_value()) ? *inputs.PrevSurfaceToGroom : surfaceToGroom;
        // What this evaluation DID, not what it was offered. A skinned surface
        // whose skeleton has no previous palette writes prev == current for
        // every root, so the frame carries no history no matter what the caller
        // asked for -- and the render pass counts a groom with HasHistory false
        // as history-rejected, which is a true statement there and was a false
        // one here.
        //
        // An UNSKINNED surface (a morph-only body) is the case this cannot
        // fold into `usePrevPose`: there is no previous palette to want, the
        // positions are the only input, and prev == current is the correct and
        // complete answer rather than a fallback. So it reports the caller's
        // own decision, which is the only thing that can invalidate it there.
        stats.HasHistory = inputs.HasHistory && (!skinned || usePrevPose);

        // ── THE SURFACE, SKINNED ONCE (#1533 E1) ─────────────────────────────
        //
        // Every root skins its triangle's three corners, for this frame and the
        // last, and a corner is shared by the roots of every triangle round it:
        // on the showcase dog 299k roots read ~1.8M corners of a ~125k-vertex
        // body, and doing them one root at a time was the frame's largest CPU
        // cost (~60 ms). When the roots outnumber the vertices, every vertex is
        // skinned ONCE into a scratch pose, in parallel, and each root reads its
        // corners from it: the same function on the same inputs, so the same
        // bits. A handful of roots on a big body keeps the per-root path, which
        // then touches fewer vertices.
        const u32 rootsToEvaluate =
            selectedCurves.has_value() ? static_cast<u32>(selectedCurves->size()) : curveCount;
        const u32 vertexCount = inputs.Surface.VertexCount;
        const bool preSkin = skinned && (static_cast<u64>(rootsToEvaluate) > vertexCount);
        // Scratch, reused across frames and grooms; thread_local so two callers
        // on two threads never share one.
        thread_local std::vector<glm::vec3> s_Current;
        thread_local std::vector<glm::vec3> s_Previous;
        thread_local std::vector<u8> s_Weighted;
        if (preSkin)
        {
            s_Current.resize(vertexCount);
            s_Weighted.resize(vertexCount);
            if (usePrevPose)
            {
                s_Previous.resize(vertexCount);
            }
            std::vector<glm::vec3>& current = s_Current;
            std::vector<glm::vec3>& previous = s_Previous;
            std::vector<u8>& weightedFlags = s_Weighted;
            ParallelFor("GroomSkinSurface", static_cast<i32>(vertexCount), 2048,
                        [&](i32 index)
                        {
                            const u32 v = static_cast<u32>(index);
                            const glm::vec3 rest = inputs.Surface.Position(v);
                            bool weighted = false;
                            current[v] = SkinGroomSurfaceVertex(inputs.Skinning, v, rest, inputs.Skinning.Palette, weighted);
                            weightedFlags[v] = weighted ? 1u : 0u;
                            if (usePrevPose)
                            {
                                bool unused = false;
                                previous[v] = SkinGroomSurfaceVertex(inputs.Skinning, v, rest, inputs.Skinning.PrevPalette, unused);
                            }
                        });
        }

        // The workers read the CALLING thread's scratch through these: a
        // thread_local named inside the body would be each worker's own, empty.
        const glm::vec3* const skinnedCurrent = preSkin ? s_Current.data() : nullptr;
        const glm::vec3* const skinnedPrevious = (preSkin && usePrevPose) ? s_Previous.data() : nullptr;
        const u8* const skinnedWeighted = preSkin ? s_Weighted.data() : nullptr;

        const auto evaluateOne = [&](u32 curve, GroomDeformationStats& rootStats)
        {
            const GroomRootBinding& record = binding.GetRoot(curve);
            if (!inputs.Surface.TriangleInRange(record.TriangleIndex))
            {
                ++rootStats.RootsSkippedOutOfRange;
                return;
            }

            const glm::uvec3 corners = inputs.Surface.TriangleIndices(record.TriangleIndex);
            const glm::vec3 rest0 = inputs.Surface.Position(corners.x);
            const glm::vec3 rest1 = inputs.Surface.Position(corners.y);
            const glm::vec3 rest2 = inputs.Surface.Position(corners.z);

            glm::vec3 current0 = rest0;
            glm::vec3 current1 = rest1;
            glm::vec3 current2 = rest2;
            if (preSkin)
            {
                current0 = skinnedCurrent[corners.x];
                current1 = skinnedCurrent[corners.y];
                current2 = skinnedCurrent[corners.z];
                rootStats.VerticesUnweighted += (skinnedWeighted[corners.x] != 0u ? 0u : 1u) +
                                                (skinnedWeighted[corners.y] != 0u ? 0u : 1u) +
                                                (skinnedWeighted[corners.z] != 0u ? 0u : 1u);
            }
            else if (skinned)
            {
                bool weighted = false;
                current0 = SkinGroomSurfaceVertex(inputs.Skinning, corners.x, rest0, inputs.Skinning.Palette, weighted);
                rootStats.VerticesUnweighted += weighted ? 0u : 1u;
                current1 = SkinGroomSurfaceVertex(inputs.Skinning, corners.y, rest1, inputs.Skinning.Palette, weighted);
                rootStats.VerticesUnweighted += weighted ? 0u : 1u;
                current2 = SkinGroomSurfaceVertex(inputs.Skinning, corners.z, rest2, inputs.Skinning.Palette, weighted);
                rootStats.VerticesUnweighted += weighted ? 0u : 1u;
            }

            // Into the groom's object space before the frame is built, so
            // the frame composes directly with the strand's own points.
            const GroomSurfaceFrame frame =
                MakeGroomSurfaceFrame(glm::vec3(surfaceToGroom * glm::vec4(current0, 1.0f)),
                                      glm::vec3(surfaceToGroom * glm::vec4(current1, 1.0f)),
                                      glm::vec3(surfaceToGroom * glm::vec4(current2, 1.0f)),
                                      record.Barycentric);
            if (!frame.Valid)
            {
                // Held at rest, and counted. See GroomRootTransform on why the
                // strand is not sent somewhere plausible instead.
                //
                // `Held` is set so this is distinguishable from a curve that was
                // never evaluated: both leave Valid false, and a consumer that
                // conflated them — the binding preview did — reports a
                // degeneracy for every strand the budget simply did not select.
                outTransforms[curve].Held = true;
                ++rootStats.RootsHeldDegenerate;
                return;
            }

            GroomRootTransform& transform = outTransforms[curve];
            transform.Origin = frame.Origin;
            transform.Rotation = frame.Rotation;
            transform.Valid = true;

            if (usePrevPose)
            {
                bool weighted = false;
                const glm::vec3 previous0 =
                    preSkin ? skinnedPrevious[corners.x]
                            : SkinGroomSurfaceVertex(inputs.Skinning, corners.x, rest0, inputs.Skinning.PrevPalette, weighted);
                const glm::vec3 previous1 =
                    preSkin ? skinnedPrevious[corners.y]
                            : SkinGroomSurfaceVertex(inputs.Skinning, corners.y, rest1, inputs.Skinning.PrevPalette, weighted);
                const glm::vec3 previous2 =
                    preSkin ? skinnedPrevious[corners.z]
                            : SkinGroomSurfaceVertex(inputs.Skinning, corners.z, rest2, inputs.Skinning.PrevPalette, weighted);
                // prevSurfaceToGroom, not surfaceToGroom: see
                // GroomDeformationInputs::PrevSurfaceToGroom.
                const GroomSurfaceFrame previousFrame =
                    MakeGroomSurfaceFrame(glm::vec3(prevSurfaceToGroom * glm::vec4(previous0, 1.0f)),
                                          glm::vec3(prevSurfaceToGroom * glm::vec4(previous1, 1.0f)),
                                          glm::vec3(prevSurfaceToGroom * glm::vec4(previous2, 1.0f)),
                                          record.Barycentric);
                if (previousFrame.Valid)
                {
                    transform.PrevOrigin = previousFrame.Origin;
                    transform.PrevRotation = previousFrame.Rotation;
                }
                else
                {
                    // A triangle that is degenerate in the PREVIOUS pose only.
                    // Aliasing to the current frame emits zero motion for this
                    // strand, which is the same answer the whole-groom
                    // no-history branch gives and is the only one that cannot
                    // smear.
                    transform.PrevOrigin = frame.Origin;
                    transform.PrevRotation = frame.Rotation;
                }
            }
            else
            {
                // prev == current, so the velocity this strand emits is exactly
                // zero. Not "approximately zero": the two go through identical
                // arithmetic in ApplyGroomRootTransform, so the difference is
                // bit-for-bit nothing.
                transform.PrevOrigin = frame.Origin;
                transform.PrevRotation = frame.Rotation;
            }

            ++rootStats.RootsDeformed;
        };

        // The roots in parallel (#1533 E1): each writes only its own curve's
        // transform, and counts into its worker's stats, summed after. An EMPTY
        // selection evaluates nothing, which is the whole point of the
        // optional -- see the header.
        TArray<GroomDeformationStats> workerStats;
        ParallelForWithTaskContext("GroomRootTransforms", workerStats, static_cast<i32>(rootsToEvaluate),
                                   [&](GroomDeformationStats& local, i32 index)
                                   {
                                       const u32 curve = selectedCurves.has_value()
                                                             ? (*selectedCurves)[static_cast<sizet>(index)]
                                                             : static_cast<u32>(index);
                                       if (curve < curveCount)
                                       {
                                           evaluateOne(curve, local);
                                       }
                                   });
        for (const GroomDeformationStats& local : workerStats)
        {
            stats.RootsDeformed += local.RootsDeformed;
            stats.RootsHeldDegenerate += local.RootsHeldDegenerate;
            stats.RootsSkippedOutOfRange += local.RootsSkippedOutOfRange;
            stats.VerticesUnweighted += local.VerticesUnweighted;
        }

        return stats;
    }
} // namespace OloEngine
