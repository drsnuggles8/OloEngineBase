#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OloEngine::Animation
{
    static glm::vec3 SafeNormalize(const glm::vec3& v)
    {
        f32 len2 = glm::length2(v);
        if (len2 > 1e-12f)
        {
            return v * glm::inversesqrt(len2);
        }
        return glm::vec3(0.0f);
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

        auto boneCount = std::min(pose.size(), parentIndices.size());

        // Save original rotations for final weight blending
        std::vector<glm::quat> originalRotations;
        bool needWeightBlend = (params.Weight < 1.0f - 1e-6f);
        if (needWeightBlend)
        {
            originalRotations.resize(boneCount);
            for (sizet i = 0; i < boneCount; ++i)
            {
                originalRotations[i] = pose[i].Rotation;
            }
        }

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
        std::reverse(boneIndices.begin(), boneIndices.end());

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
        for (u32 iter = 0; iter < params.MaxIterations; ++iter)
        {
            if (glm::length2(jointPositions.back() - params.TargetPosition) < params.ConvergenceThreshold)
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
                if (len > params.ConvergenceThreshold)
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
                if (len > params.ConvergenceThreshold)
                {
                    jointPositions[i] = jointPositions[i - 1] + (dir / len) * boneLengths[i];
                }
                else
                {
                    jointPositions[i] = jointPositions[i - 1];
                }
            }
        }

        // --- Convert new positions back to local rotations ---
        // For each bone except the last (end-effector), rotate it so
        // the direction to the next bone matches the FABRIK solution.
        // The end-effector's position is determined by the parent chain.

        // Helper to get the decomposed pre-transform for a bone index
        auto getPreTransform = [&](u32 idx) -> BoneTransform
        {
            if (idx < preTransforms.size())
            {
                static constexpr glm::mat4 kIdentity{1.0f};
                if (std::memcmp(&preTransforms[idx], &kIdentity, sizeof(glm::mat4)) != 0)
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

        // Apply global weight: blend between original and IK result
        if (needWeightBlend)
        {
            for (sizet i = 0; i < boneCount; ++i)
            {
                pose[i].Rotation = glm::slerp(originalRotations[i], pose[i].Rotation, params.Weight);
            }
        }
    }

} // namespace OloEngine::Animation
