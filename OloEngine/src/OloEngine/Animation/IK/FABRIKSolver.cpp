#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/FABRIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Animation
{
    static glm::vec3 SafeNormalize(const glm::vec3& v)
    {
        if (f32 len2 = glm::length2(v); len2 > 1e-12f)
        {
            return v * glm::inversesqrt(len2);
        }
        return glm::vec3(0.0f);
    }

    void FABRIKSolver::Solve(
        std::span<BoneTransform> pose,
        std::span<const int> parentIndices,
        const FABRIKParams& params,
        std::span<const glm::mat4> preTransforms)
    {
        OLO_PROFILE_FUNCTION();

        if (params.ChainLength < 2 || params.TargetBoneIndex >= pose.size())
        {
            return;
        }

        // Validate floating-point parameters
        if (!std::isfinite(params.TargetPosition.x) || !std::isfinite(params.TargetPosition.y) || !std::isfinite(params.TargetPosition.z) || !std::isfinite(params.PoleVector.x) || !std::isfinite(params.PoleVector.y) || !std::isfinite(params.PoleVector.z) || !std::isfinite(params.Weight) || !std::isfinite(params.Tolerance))
        {
            return;
        }

        auto weight = glm::clamp(params.Weight, 0.0f, 1.0f);
        auto tolerance = std::max(params.Tolerance, 0.0f);

        auto boneCount = std::min(pose.size(), parentIndices.size());

        // Build the chain of bone indices from end-effector up to chain root
        std::vector<u32> boneIndices;
        boneIndices.reserve(std::min<sizet>(params.ChainLength, boneCount));

        {
            auto idx = params.TargetBoneIndex;
            for (u32 i = 0; i < params.ChainLength && idx < static_cast<u32>(boneCount); ++i)
            {
                boneIndices.push_back(idx);
                auto parent = parentIndices[idx];
                if (parent < 0)
                {
                    break;
                }
                idx = static_cast<u32>(parent);
            }
        }

        if (boneIndices.size() < 2)
        {
            return; // Need at least 2 bones for FABRIK
        }

        // Reverse so indices go root-to-tip (ascending from chain root to end-effector)
        std::ranges::reverse(boneIndices);

        // Save original rotations for chain bones only (for final weight blending)
        std::vector<glm::quat> originalRotations;
        bool needWeightBlend = (weight < 1.0f - 1e-6f);
        if (needWeightBlend)
        {
            originalRotations.resize(boneIndices.size());
            for (sizet i = 0; i < boneIndices.size(); ++i)
            {
                originalRotations[i] = pose[boneIndices[i]].Rotation;
            }
        }

        // Compute model-space transforms (including pre-transforms for correct coordinate space)
        std::vector<BoneTransform> modelSpace;
        BlendUtils::ComputeModelSpacePose(pose, parentIndices, modelSpace, preTransforms);

        // Joint positions in model space (one per chain bone)
        auto jointCount = static_cast<u32>(boneIndices.size());
        std::vector<glm::vec3> jointPositions(jointCount);
        std::vector<f32> boneLengths(jointCount, 0.0f);

        for (sizet i = 0; i < jointCount; ++i)
        {
            jointPositions[i] = modelSpace[boneIndices[i]].Translation;
        }

        f32 totalLength = 0.0f;
        for (sizet i = 1; i < jointCount; ++i)
        {
            boneLengths[i] = glm::length(jointPositions[i] - jointPositions[i - 1]);
            totalLength += boneLengths[i];
        }

        constexpr f32 kDirectionEpsilon = 1e-6f;
        if (totalLength < kDirectionEpsilon)
        {
            return; // All joints coincident — no usable chain geometry
        }

        auto basePosition = jointPositions[0];

        if (glm::length(params.TargetPosition - basePosition) >= totalLength)
        {
            // Unreachable target: straighten the chain toward it directly —
            // this is the exact FABRIK fixed point, no iteration needed.
            auto dir = SafeNormalize(params.TargetPosition - basePosition);
            if (glm::length2(dir) < kDirectionEpsilon)
            {
                return;
            }
            for (sizet i = 1; i < jointCount; ++i)
            {
                jointPositions[i] = jointPositions[i - 1] + dir * boneLengths[i];
            }
        }
        else
        {
            // --- FABRIK iterations ---
            f32 tolerance2 = tolerance * tolerance;
            for (u32 iter = 0; iter < params.MaxIterations; ++iter)
            {
                if (glm::length2(jointPositions.back() - params.TargetPosition) < tolerance2)
                {
                    break;
                }

                // Backward pass: move end-effector to target, pull chain backward
                jointPositions.back() = params.TargetPosition;
                for (auto i = static_cast<i32>(jointCount) - 1; i > 0; --i)
                {
                    auto idx = static_cast<sizet>(i);
                    auto dir = jointPositions[idx - 1] - jointPositions[idx];
                    f32 len = glm::length(dir);
                    if (len > kDirectionEpsilon)
                    {
                        jointPositions[idx - 1] = jointPositions[idx] + (dir / len) * boneLengths[idx];
                    }
                    else
                    {
                        jointPositions[idx - 1] = jointPositions[idx];
                    }
                }

                // Forward pass: pin root, push chain forward
                jointPositions[0] = basePosition;
                for (sizet i = 1; i < jointCount; ++i)
                {
                    auto dir = jointPositions[i] - jointPositions[i - 1];
                    f32 len = glm::length(dir);
                    if (len > kDirectionEpsilon)
                    {
                        jointPositions[i] = jointPositions[i - 1] + (dir / len) * boneLengths[i];
                    }
                    else
                    {
                        jointPositions[i] = jointPositions[i - 1];
                    }
                }
            }
        }

        // --- Optional pole constraint ---
        // Rotate each intermediate joint around the axis through its two
        // neighbours so it lies on the half-plane containing the pole vector.
        // Both adjacent bone lengths are preserved exactly (the neighbours lie
        // on the rotation axis), and the root/tip never move.
        if (glm::length2(params.PoleVector) > kDirectionEpsilon && jointCount >= 3)
        {
            for (sizet i = 1; i + 1 < jointCount; ++i)
            {
                auto axis = SafeNormalize(jointPositions[i + 1] - jointPositions[i - 1]);
                if (glm::length2(axis) < kDirectionEpsilon)
                {
                    continue;
                }

                auto toJoint = jointPositions[i] - jointPositions[i - 1];
                auto toPole = params.PoleVector - jointPositions[i - 1];
                auto projJoint = toJoint - axis * glm::dot(toJoint, axis);
                auto projPole = toPole - axis * glm::dot(toPole, axis);

                if (glm::length2(projJoint) > kDirectionEpsilon && glm::length2(projPole) > kDirectionEpsilon)
                {
                    auto from = SafeNormalize(projJoint);
                    auto to = SafeNormalize(projPole);
                    f32 angle = std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f));
                    if (glm::dot(glm::cross(from, to), axis) < 0.0f)
                    {
                        angle = -angle;
                    }
                    jointPositions[i] = jointPositions[i - 1] + glm::angleAxis(angle, axis) * toJoint;
                }
            }
        }

        // --- Convert new positions back to local rotations ---
        // For each bone except the last (end-effector), rotate it so
        // the direction to the next bone matches the FABRIK solution.
        // The end-effector's position is determined by the parent chain.

        // Helper to get the decomposed pre-transform for a bone index
        auto getPreTransform = [&preTransforms](u32 idx) -> BoneTransform
        {
            if (idx < preTransforms.size())
            {
                static constexpr glm::mat4 kIdentity{ 1.0f };
                if (!Math::BitwiseEqual(preTransforms[idx], kIdentity))
                {
                    return BlendUtils::DecomposeMatrix(preTransforms[idx]);
                }
            }
            return {};
        };

        u32 chainRootBone = boneIndices[0];
        auto chainRootParent = parentIndices[chainRootBone];
        BoneTransform parentModelSpace;
        if (chainRootParent >= 0 && static_cast<sizet>(chainRootParent) < boneCount)
        {
            parentModelSpace = modelSpace[static_cast<sizet>(chainRootParent)];
        }

        for (u32 i = 0; i + 1 < jointCount; ++i)
        {
            u32 thisIdx = boneIndices[i];
            auto pre = getPreTransform(thisIdx);
            auto thisBoneMS = BlendUtils::MultiplyTransforms(parentModelSpace, BlendUtils::MultiplyTransforms(pre, pose[thisIdx]));

            // Original direction to next bone (before correction)
            auto preNext = getPreTransform(boneIndices[i + 1]);
            auto nextBoneMS = BlendUtils::MultiplyTransforms(thisBoneMS, BlendUtils::MultiplyTransforms(preNext, pose[boneIndices[i + 1]]));
            auto originalDir = nextBoneMS.Translation - thisBoneMS.Translation;

            // Target direction from FABRIK solution (using actual corrected position)
            auto targetDir = jointPositions[i + 1] - thisBoneMS.Translation;

            // Compute rotation in bone's local frame
            auto invRot = glm::conjugate(thisBoneMS.Rotation);
            auto toOriginal = SafeNormalize(invRot * originalDir);
            auto toTarget = SafeNormalize(invRot * targetDir);

            if (glm::length2(toOriginal) > 1e-10f && glm::length2(toTarget) > 1e-10f)
            {
                pose[thisIdx].Rotation *= glm::rotation(toOriginal, toTarget);
            }

            // Update parent for next iteration (chain bones are in order)
            parentModelSpace = BlendUtils::MultiplyTransforms(parentModelSpace, BlendUtils::MultiplyTransforms(pre, pose[thisIdx]));
        }

        // Apply global weight: blend between original and IK result (chain bones only)
        if (needWeightBlend)
        {
            for (sizet i = 0; i < boneIndices.size(); ++i)
            {
                auto bi = boneIndices[i];
                pose[bi].Rotation = glm::slerp(originalRotations[i], pose[bi].Rotation, weight);
            }
        }
    }

} // namespace OloEngine::Animation
