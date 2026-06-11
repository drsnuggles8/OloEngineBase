#include "OloEnginePCH.h"
#include "OloEngine/Animation/Procedural/SpringBoneSolver.h"
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
    namespace
    {
        // Clamp the integration step so a frame hitch can't make the Verlet
        // step overshoot into instability. 1/30 s matches the slowest frame
        // the simulation is expected to look correct at.
        constexpr f32 kMaxDeltaTime = 1.0f / 30.0f;
        constexpr f32 kDirectionEpsilon2 = 1e-12f;

        glm::vec3 SafeNormalize(const glm::vec3& v)
        {
            if (f32 len2 = glm::length2(v); len2 > kDirectionEpsilon2)
            {
                return v * glm::inversesqrt(len2);
            }
            return glm::vec3(0.0f);
        }

        bool IsFiniteVec3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
    } // namespace

    void SpringBoneSolver::Solve(
        std::span<BoneTransform> pose,
        std::span<const int> parentIndices,
        const SpringBoneParams& params,
        SpringBoneState& state,
        f32 deltaTime,
        std::span<const glm::mat4> preTransforms)
    {
        OLO_PROFILE_FUNCTION();

        if (params.ChainLength < 2 || params.EndBoneIndex >= pose.size())
        {
            return;
        }

        // Validate floating-point parameters — reject the whole solve rather
        // than feeding NaN/Inf into the persistent state.
        if (!std::isfinite(params.Stiffness) || !std::isfinite(params.Damping) || !std::isfinite(params.Weight) || !IsFiniteVec3(params.Gravity) || !std::isfinite(deltaTime))
        {
            return;
        }

        auto weight = glm::clamp(params.Weight, 0.0f, 1.0f);
        if (weight <= 0.0f)
        {
            return;
        }

        auto stiffness = std::max(params.Stiffness, 0.0f);
        auto damping = std::max(params.Damping, 0.0f);

        auto boneCount = std::min(pose.size(), parentIndices.size());

        // Build the chain of bone indices from the tip up to the chain root.
        std::vector<u32> boneIndices;
        boneIndices.reserve(params.ChainLength);
        {
            auto idx = params.EndBoneIndex;
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
            return; // Need at least an anchor bone and one simulated joint
        }

        // Reverse so indices go root-to-tip
        std::ranges::reverse(boneIndices);

        // Animated (rest) joint positions in model space for this frame
        std::vector<BoneTransform> modelSpace;
        BlendUtils::ComputeModelSpacePose(pose, parentIndices, modelSpace, preTransforms);

        auto jointCount = static_cast<u32>(boneIndices.size());
        std::vector<glm::vec3> restPositions(jointCount);
        for (sizet i = 0; i < jointCount; ++i)
        {
            restPositions[i] = modelSpace[boneIndices[i]].Translation;
        }

        // Segment lengths from the animated pose (recomputed per frame so
        // animated scale/translation changes propagate into the constraint)
        std::vector<f32> segmentLengths(jointCount, 0.0f);
        for (sizet i = 1; i < jointCount; ++i)
        {
            segmentLengths[i] = glm::length(restPositions[i] - restPositions[i - 1]);
        }

        // Simulated joints are 1..jointCount-1; joint 0 is the anchor whose
        // position is driven by the parent hierarchy, not the simulation.
        auto simCount = static_cast<sizet>(jointCount) - 1;

        auto stateValid = state.Initialized && state.CurrPositions.size() == simCount && state.PrevPositions.size() == simCount;
        if (stateValid)
        {
            for (sizet i = 0; i < simCount && stateValid; ++i)
            {
                stateValid = IsFiniteVec3(state.CurrPositions[i]) && IsFiniteVec3(state.PrevPositions[i]);
            }
        }

        if (!stateValid)
        {
            // (Re-)initialize to the animated pose: the chain starts at rest,
            // so the first solved frame leaves the pose untouched.
            state.CurrPositions.assign(restPositions.begin() + 1, restPositions.end());
            state.PrevPositions.assign(restPositions.begin() + 1, restPositions.end());
            state.Initialized = true;
            return;
        }

        if (deltaTime <= 0.0f)
        {
            return;
        }

        auto dt = std::min(deltaTime, kMaxDeltaTime);
        auto dt2 = dt * dt;
        auto velocityRetain = glm::clamp(1.0f - damping * dt, 0.0f, 1.0f);

        // --- Verlet integration, root-to-tip so each joint constrains
        // against its parent's already-simulated position ---
        std::vector<glm::vec3> simPositions(jointCount);
        simPositions[0] = restPositions[0];
        for (sizet j = 1; j < jointCount; ++j)
        {
            auto si = j - 1; // index into the state arrays
            const auto& curr = state.CurrPositions[si];
            const auto& prev = state.PrevPositions[si];

            auto velocity = (curr - prev) * velocityRetain;
            auto accel = stiffness * (restPositions[j] - curr) + params.Gravity;
            auto next = curr + velocity + accel * dt2;

            // Constrain to the animated segment length around the parent joint
            const auto& parentPos = simPositions[j - 1];
            auto dir = SafeNormalize(next - parentPos);
            if (glm::length2(dir) <= kDirectionEpsilon2)
            {
                // Degenerate: fall back to the animated direction
                dir = SafeNormalize(restPositions[j] - restPositions[j - 1]);
            }
            next = parentPos + dir * segmentLengths[j];

            state.PrevPositions[si] = curr;
            state.CurrPositions[si] = next;
            simPositions[j] = next;
        }

        // --- Convert simulated positions back to local rotations ---
        // For each bone except the tip, rotate it so the direction to its
        // child matches the simulation (same pattern as LimbIKSolver).
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

            // Animated direction to the child (before correction)
            auto preNext = getPreTransform(boneIndices[i + 1]);
            auto nextBoneMS = BlendUtils::MultiplyTransforms(thisBoneMS, BlendUtils::MultiplyTransforms(preNext, pose[boneIndices[i + 1]]));
            auto originalDir = nextBoneMS.Translation - thisBoneMS.Translation;

            // Simulated direction (using the actual corrected parent position)
            auto targetDir = simPositions[i + 1] - thisBoneMS.Translation;

            // Compute the correction in the bone's local frame
            auto invRot = glm::conjugate(thisBoneMS.Rotation);
            auto toOriginal = SafeNormalize(invRot * originalDir);
            auto toTarget = SafeNormalize(invRot * targetDir);

            if (glm::length2(toOriginal) > 1e-10f && glm::length2(toTarget) > 1e-10f)
            {
                pose[thisIdx].Rotation *= glm::rotation(toOriginal, toTarget);
            }

            // Update the parent transform for the next chain bone
            parentModelSpace = BlendUtils::MultiplyTransforms(parentModelSpace, BlendUtils::MultiplyTransforms(pre, pose[thisIdx]));
        }

        // Apply global weight: blend between the animated and simulated result
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
