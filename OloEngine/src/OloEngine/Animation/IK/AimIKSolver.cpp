#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/AimIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::Animation
{
    // When there's an offset, recompute the forward vector so that the point at
    // the offset position aims at the target. The vector starts at joint origin
    // and ends on a line perpendicular to pivot-offset, at the intersection with
    // the sphere defined by the target position.
    // Derived from ozz-animation and used in Hazel's AimIK.
    static bool ComputeOffsettedForward(
        const glm::vec3& forward,
        const glm::vec3& offset,
        const glm::vec3& target,
        glm::vec3& outOffsettedForward)
    {
        // Normalize forward to ensure unit-length for projection math
        f32 forwardLen2 = glm::length2(forward);
        if (forwardLen2 < 1e-12f)
        {
            return false;
        }
        auto fwd = forward * glm::inversesqrt(forwardLen2);

        // AO is projected offset onto normalized forward
        f32 AOl = glm::dot(fwd, offset);

        // Square length of perpendicular component
        f32 ACl2 = glm::length2(offset) - AOl * AOl;

        // Square length of target vector (sphere radius²)
        f32 r2 = glm::length2(target);

        // If offset is outside the sphere, target is unreachable
        if (ACl2 > r2)
        {
            return false;
        }

        // Distance from offset projection to sphere intersection
        f32 AIl = std::sqrt(r2 - ACl2);

        outOffsettedForward = offset + (AIl - AOl) * fwd;
        return true;
    }

    // Build a quaternion from a rotation axis and cosine of the rotation angle.
    // The axis must be non-zero length.
    static glm::quat FromAxisAndCosAngle(const glm::vec3& axis, f32 cosAngle)
    {
        f32 halfCos2 = 0.5f * (1.0f + cosAngle);
        f32 halfSin2 = 1.0f - halfCos2;
        f32 halfCos = std::sqrt(std::max(halfCos2, 0.0f));
        f32 halfSin = std::sqrt(std::max(halfSin2, 0.0f));
        return { halfCos, halfSin * axis };
    }

    // Compute the bone-local correction quaternion that aims `forward` (with pivot
    // `offset`) at `boneToTarget`, twisted so the joint's up axis lines up with the
    // pole vector. Returns identity for degenerate geometry. jointIndex is the
    // chain position (last joint gets full weight, others get ChainFactor).
    static glm::quat ComputeAimJointCorrection(
        const glm::vec3& forward,
        const glm::vec3& offset,
        const glm::vec3& boneToTarget,
        const glm::vec3& up,
        const BoneTransform& invBone,
        const AimIKParams& params,
        u32 jointIndex,
        u32 actualChainLength)
    {
        f32 boneToTargetLen2 = glm::length2(boneToTarget);
        glm::vec3 offsettedForward;
        glm::quat correction = glm::identity<glm::quat>();

        if (constexpr f32 kEpsilon = 1e-7f; boneToTargetLen2 > kEpsilon && ComputeOffsettedForward(forward, offset, boneToTarget, offsettedForward))
        {
            // Quaternion that rotates offsetted forward onto target direction
            glm::quat boneToTargetRotation(offsettedForward, boneToTarget);

            // Align joint up to the pole vector
            auto correctedUp = boneToTargetRotation * up;

            // Compute reference plane normal and bone plane normal
            auto poleVec = BlendUtils::TransformVector(invBone, params.PoleVector);
            auto refBoneNormal = glm::cross(poleVec, boneToTarget);
            auto boneNormal = glm::cross(correctedUp, boneToTarget);
            f32 refBoneNormalLen2 = glm::length2(refBoneNormal);
            f32 boneNormalLen2 = glm::length2(boneNormal);

            glm::quat planeRotation = glm::identity<glm::quat>();
            if ((boneToTargetLen2 > kEpsilon) && (boneNormalLen2 > kEpsilon) && (refBoneNormalLen2 > kEpsilon))
            {
                auto rsqrts = glm::inversesqrt(glm::vec3{ boneToTargetLen2, boneNormalLen2, refBoneNormalLen2 });

                auto planeRotationAxis = rsqrts.x * boneToTarget;

                f32 planeRotationCosAngle = glm::clamp(
                    glm::dot(boneNormal * rsqrts.y, refBoneNormal * rsqrts.z),
                    -1.0f, 1.0f);
                bool axisFlip = glm::dot(refBoneNormal, correctedUp) < 0.0f;
                auto planeRotationAxisFlipped = axisFlip ? -planeRotationAxis : planeRotationAxis;

                planeRotation = FromAxisAndCosAngle(planeRotationAxisFlipped, planeRotationCosAngle);
            }

            correction = planeRotation * boneToTargetRotation;

            // Ensure w is positive for correct NLerp direction
            if (correction.w < 0.0f)
            {
                correction *= -1.0f;
            }

            // Apply chain factor: last bone gets full weight, others get chainFactor
            f32 weight = (jointIndex == actualChainLength - 1) ? 1.0f : params.ChainFactor;
            if (weight < 1.0f)
            {
                correction = glm::normalize(glm::mix(glm::identity<glm::quat>(), correction, weight));
            }
        }
        return correction;
    }

    void AimIKSolver::Solve(
        std::span<BoneTransform> pose,
        std::span<const int> parentIndices,
        const AimIKParams& params,
        std::span<const glm::mat4> preTransforms)
    {
        OLO_PROFILE_FUNCTION();

        if (params.ChainLength == 0 || params.TargetBoneIndex >= pose.size())
        {
            return;
        }

        auto boneCount = std::min(pose.size(), parentIndices.size());

        // Save original rotations for chain bones only (for final weight blending)
        std::vector<u32> chainIndices;
        std::vector<glm::quat> originalRotations;
        bool needWeightBlend = (params.Weight < 1.0f - 1e-6f);
        if (needWeightBlend)
        {
            // Pre-compute chain indices (end-effector walking up parents)
            auto idx = params.TargetBoneIndex;
            for (u32 i = 0; i < params.ChainLength && idx < static_cast<u32>(boneCount); ++i)
            {
                chainIndices.push_back(idx);
                auto parent = parentIndices[idx];
                if (parent < 0)
                {
                    break;
                }
                idx = static_cast<u32>(parent);
            }
            auto chainSize = chainIndices.size();
            originalRotations.resize(chainSize);
            for (sizet i = 0; i < chainSize; ++i)
            {
                originalRotations[i] = pose[chainIndices[i]].Rotation;
            }
        }

        // Compute model-space transforms (including pre-transforms for correct coordinate space)
        std::vector<BoneTransform> modelSpace;
        BlendUtils::ComputeModelSpacePose(pose, parentIndices, modelSpace, preTransforms);

        // Iterative AimIK from child to parent.
        // Based on ozz-animation's algorithm (also used in Hazel).
        //
        // For the first joint, aim IK is applied with the global forward and
        // offset. For remaining joints, forward and offset are corrected by the
        // result of the previous joint and brought back into bone-local space.
        // The last joint always receives full weight to guarantee convergence.
        u32 prevBoneIndex = static_cast<u32>(-1);
        auto boneIndex = params.TargetBoneIndex;
        glm::vec3 offset;
        glm::vec3 forward;
        glm::quat correction = glm::identity<glm::quat>();

        // Count actual chain length for weight calculation (may be shorter than
        // params.ChainLength if we hit the root before exhausting the chain)
        u32 actualChainLength = 0;
        {
            auto idx = params.TargetBoneIndex;
            for (u32 c = 0; c < params.ChainLength && idx < static_cast<u32>(boneCount); ++c)
            {
                ++actualChainLength;
                auto p = parentIndices[idx];
                if (p < 0)
                    break;
                idx = static_cast<u32>(p);
            }
        }

        for (u32 i = 0; i < params.ChainLength && boneIndex < static_cast<u32>(boneCount); ++i)
        {
            auto invBone = BlendUtils::InverseTransform(modelSpace[boneIndex]);
            auto up = BlendUtils::TransformVector(invBone, params.PoleVector);

            if (i == 0)
            {
                // First joint: use global aim axis and offset directly
                offset = params.AimOffset;
                forward = params.AimAxis;
            }
            else
            {
                // Apply previous correction, bring to model space, then to this bone's local space
                auto correctedForward = BlendUtils::TransformVector(modelSpace[prevBoneIndex], correction * forward);
                auto correctedOffset = BlendUtils::TransformPoint(modelSpace[prevBoneIndex], correction * offset);

                forward = BlendUtils::TransformVector(invBone, correctedForward);
                offset = BlendUtils::TransformPoint(invBone, correctedOffset);
            }

            // Joint-to-target in bone-local space, then compute the aim correction.
            auto boneToTarget = BlendUtils::TransformPoint(invBone, params.TargetPosition);
            correction = ComputeAimJointCorrection(forward, offset, boneToTarget, up, invBone, params, i, actualChainLength);

            // Apply correction to the bone's local rotation.
            // The correction is in bone-local space (post all transforms), so
            // it must be post-multiplied: new_local = local * correction.
            // This ensures ModelSpace_new = Parent * Pre * (Local * C) = ModelSpace_old * C.
            pose[boneIndex].Rotation = pose[boneIndex].Rotation * correction;

            // Walk up the parent chain
            prevBoneIndex = boneIndex;
            auto parent = parentIndices[boneIndex];
            if (parent < 0)
            {
                break;
            }
            boneIndex = static_cast<u32>(parent);
        }

        // Apply global weight: blend between original and IK result (chain bones only)
        if (needWeightBlend)
        {
            auto blendCount = chainIndices.size();
            for (sizet i = 0; i < blendCount; ++i)
            {
                auto bi = chainIndices[i];
                pose[bi].Rotation = glm::slerp(originalRotations[i], pose[bi].Rotation, params.Weight);
            }
        }
    }

} // namespace OloEngine::Animation
