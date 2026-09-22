#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomDeformation.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <cmath>
#include <cstring>

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
                                                      TArray<GroomRootTransform>& outTransforms)
    {
        GroomDeformationStats stats;

        const u32 curveCount = groom.GetCurveCount();
        // Fully written, never partially: an index into this array must be safe
        // for every curve whether or not it was selected — see the header.
        outTransforms.Init(GroomRootTransform{}, static_cast<i32>(curveCount));

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

        const auto evaluateOne = [&](u32 curve)
        {
            const GroomRootBinding& record = binding.GetRoot(curve);
            if (!inputs.Surface.TriangleInRange(record.TriangleIndex))
            {
                ++stats.RootsSkippedOutOfRange;
                return;
            }

            const glm::uvec3 corners = inputs.Surface.TriangleIndices(record.TriangleIndex);
            const glm::vec3 rest0 = inputs.Surface.Position(corners.x);
            const glm::vec3 rest1 = inputs.Surface.Position(corners.y);
            const glm::vec3 rest2 = inputs.Surface.Position(corners.z);

            glm::vec3 current0 = rest0;
            glm::vec3 current1 = rest1;
            glm::vec3 current2 = rest2;
            if (skinned)
            {
                bool weighted = false;
                current0 = SkinGroomSurfaceVertex(inputs.Skinning, corners.x, rest0, inputs.Skinning.Palette, weighted);
                stats.VerticesUnweighted += weighted ? 0u : 1u;
                current1 = SkinGroomSurfaceVertex(inputs.Skinning, corners.y, rest1, inputs.Skinning.Palette, weighted);
                stats.VerticesUnweighted += weighted ? 0u : 1u;
                current2 = SkinGroomSurfaceVertex(inputs.Skinning, corners.z, rest2, inputs.Skinning.Palette, weighted);
                stats.VerticesUnweighted += weighted ? 0u : 1u;
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
                ++stats.RootsHeldDegenerate;
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
                    SkinGroomSurfaceVertex(inputs.Skinning, corners.x, rest0, inputs.Skinning.PrevPalette, weighted);
                const glm::vec3 previous1 =
                    SkinGroomSurfaceVertex(inputs.Skinning, corners.y, rest1, inputs.Skinning.PrevPalette, weighted);
                const glm::vec3 previous2 =
                    SkinGroomSurfaceVertex(inputs.Skinning, corners.z, rest2, inputs.Skinning.PrevPalette, weighted);
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

            ++stats.RootsDeformed;
        };

        if (!selectedCurves.has_value())
        {
            for (u32 curve = 0; curve < curveCount; ++curve)
            {
                evaluateOne(curve);
            }
        }
        else
        {
            // An EMPTY selection evaluates nothing, which is the whole point of
            // the optional -- see the header.
            for (const u32 curve : *selectedCurves)
            {
                if (curve < curveCount)
                {
                    evaluateOne(curve);
                }
            }
        }

        return stats;
    }
} // namespace OloEngine
