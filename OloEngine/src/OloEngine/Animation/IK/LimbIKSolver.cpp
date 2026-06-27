#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

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

    // Iterative FABRIK position solve for a limb chain (in place): pin the root,
    // drag the tip onto the target. Coincident joints collapse onto the previous
    // joint (the limb variant's degenerate fallback).
    static void SolveLimbPositions(
        std::vector<glm::vec3>& jointPositions,
        const std::vector<f32>& boneLengths,
        const glm::vec3& basePosition,
        const glm::vec3& targetPosition,
        u32 jointCount,
        u32 maxIterations,
        f32 convergenceThreshold)
    {
        constexpr f32 kDirectionEpsilon = 1e-6f;
        f32 convergenceThreshold2 = convergenceThreshold * convergenceThreshold;
        for (u32 iter = 0; iter < maxIterations; ++iter)
        {
            if (glm::length2(jointPositions.back() - targetPosition) < convergenceThreshold2)
            {
                break;
            }

            // Backward pass: move end-effector to target, pull chain backward
            jointPositions.back() = targetPosition;
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

    void LimbIKSolver::Solve(
        std::span<BoneTransform> pose,
        std::span<const int> parentIndices,
        const LimbIKParams& params,
        std::span<const glm::mat4> preTransforms)
    {
        OLO_PROFILE_FUNCTION();

        if (params.ChainLength == 0 || params.TargetBoneIndex >= pose.size())
        {
            return;
        }

        // Validate floating-point parameters
        if (!std::isfinite(params.TargetPosition.x) || !std::isfinite(params.TargetPosition.y) || !std::isfinite(params.TargetPosition.z))
        {
            return;
        }
        if (!std::isfinite(params.Weight) || !std::isfinite(params.ConvergenceThreshold))
        {
            return;
        }

        // Clamp weight and convergence threshold to non-negative
        auto weight = glm::clamp(params.Weight, 0.0f, 1.0f);
        auto convergenceThreshold = std::max(params.ConvergenceThreshold, 0.0f);

        auto boneCount = std::min(pose.size(), parentIndices.size());

        // Build the chain of bone indices from end-effector up to chain root.
        // chainLength bones + 1 extra joint for the tip of the end-effector.
        u32 chainJointCount = params.ChainLength + 1;
        std::vector<u32> boneIndices;
        boneIndices.reserve(chainJointCount);

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
        // Must be captured AFTER reverse so indices align with the blend loop order.
        std::vector<glm::quat> originalRotations;
        bool needWeightBlend = (weight < 1.0f - 1e-6f);
        if (needWeightBlend)
        {
            auto chainSize = boneIndices.size();
            originalRotations.resize(chainSize);
            for (sizet i = 0; i < chainSize; ++i)
            {
                originalRotations[i] = pose[boneIndices[i]].Rotation;
            }
        }

        // Compute model-space transforms (including pre-transforms for correct coordinate space)
        std::vector<BoneTransform> modelSpace;
        BlendUtils::ComputeModelSpacePose(pose, parentIndices, modelSpace, preTransforms);

        // Joint positions in model space (one per chain bone — no extra tip)
        auto jointCount = static_cast<u32>(boneIndices.size());
        std::vector<glm::vec3> jointPositions(jointCount);
        std::vector<f32> boneLengths(jointCount, 0.0f);

        for (sizet i = 0; i < jointCount; ++i)
        {
            jointPositions[i] = modelSpace[boneIndices[i]].Translation;
        }

        // Compute bone lengths between consecutive joints
        for (sizet i = 1; i < jointCount; ++i)
        {
            boneLengths[i] = glm::length(jointPositions[i] - jointPositions[i - 1]);
        }

        auto basePosition = jointPositions[0];

        // --- FABRIK iterations ---
        SolveLimbPositions(jointPositions, boneLengths, basePosition, params.TargetPosition,
                           jointCount, params.MaxIterations, convergenceThreshold);

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

            if (auto toTarget = SafeNormalize(invRot * targetDir);
                glm::length2(toOriginal) > 1e-10f && glm::length2(toTarget) > 1e-10f)
            {
                pose[thisIdx].Rotation *= glm::rotation(toOriginal, toTarget);
            }

            // Update parent for next iteration (chain bones are in order)
            parentModelSpace = BlendUtils::MultiplyTransforms(parentModelSpace, BlendUtils::MultiplyTransforms(pre, pose[thisIdx]));
        }

        // Apply global weight: blend between original and IK result (chain bones only)
        if (needWeightBlend)
        {
            auto blendCount = boneIndices.size();
            for (sizet i = 0; i < blendCount; ++i)
            {
                auto bi = boneIndices[i];
                pose[bi].Rotation = glm::slerp(originalRotations[i], pose[bi].Rotation, weight);
            }
        }
    }

} // namespace OloEngine::Animation
