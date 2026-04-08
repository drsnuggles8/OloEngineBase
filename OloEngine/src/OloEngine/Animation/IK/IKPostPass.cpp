#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/IKPostPass.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/AimIKSolver.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Animation/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/norm.hpp>

namespace OloEngine::Animation
{
    // Marks all bones in an IK chain (from startBone walking up via parentIndices) as modified
    static void MarkIkChain(
        u32 startBone,
        u32 chainLength,
        sizet boneCount,
        std::span<const int> parentIndices,
        std::vector<bool>& ikModified)
    {
        auto bone = startBone;
        for (u32 j = 0; j < chainLength && bone < static_cast<u32>(boneCount); ++j)
        {
            ikModified[bone] = true;
            if (auto parent = parentIndices[bone]; parent < 0)
            {
                break;
            }
            else
            {
                bone = static_cast<u32>(parent);
            }
        }
    }

    void ApplyIKPostPass(
        Skeleton& skeleton,
        const IKTargetComponent& ikTarget,
        const glm::mat4& entityWorldTransform)
    {
        OLO_PROFILE_FUNCTION();

        if (!ikTarget.AimIKEnabled && !ikTarget.LimbIKEnabled)
        {
            return;
        }

        // Derive bone count from the skeleton itself to avoid stale caller values
        auto boneCount = skeleton.m_LocalTransforms.size();
        if (boneCount == 0)
        {
            return;
        }
        OLO_CORE_ASSERT(skeleton.m_ParentIndices.size() == boneCount, "ParentIndices/LocalTransforms size mismatch");
        OLO_CORE_ASSERT(skeleton.m_BonePreTransforms.empty() || skeleton.m_BonePreTransforms.size() == boneCount,
                        "BonePreTransforms size mismatch");

        // Track which bones the IK chains will modify so we only write those
        // back — avoids the lossy glm::decompose round-trip on untouched bones.
        std::vector<bool> ikModified(boneCount, false);

        // Decompose mat4 local transforms to BoneTransform for IK solvers
        std::vector<BoneTransform> localPose(boneCount);
        for (sizet i = 0; i < boneCount; ++i)
        {
            glm::vec3 scale;
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 skew;
            glm::vec4 perspective;
            if (!glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective))
            {
                localPose[i] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
                continue;
            }
            localPose[i] = { translation, rotation, scale };
        }

        auto isFiniteVec3 = [](const glm::vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };

        constexpr f32 kVecEpsilon = 1e-8f;

        if (ikTarget.AimIKEnabled && ikTarget.AimBoneIndex < static_cast<u32>(boneCount))
        {
            // Validate all floating-point inputs individually for clarity
            bool validInputs = isFiniteVec3(ikTarget.AimTarget) && isFiniteVec3(ikTarget.AimAxis) && isFiniteVec3(ikTarget.AimOffset) && isFiniteVec3(ikTarget.AimPoleVector) && std::isfinite(ikTarget.AimWeight) && std::isfinite(ikTarget.AimChainFactor);
            bool validDirections = glm::length2(ikTarget.AimAxis) > kVecEpsilon && glm::length2(ikTarget.AimPoleVector) > kVecEpsilon;

            if (validInputs && validDirections)
            {
                // Skip if clamped weight is zero — no visible effect
                if (auto aimWeight = glm::clamp(ikTarget.AimWeight, 0.0f, 1.0f); aimWeight > 0.0f)
                {
                    auto aimChain = std::clamp(ikTarget.AimChainLength, 1u, static_cast<u32>(boneCount));
                    MarkIkChain(ikTarget.AimBoneIndex, aimChain, boneCount, skeleton.m_ParentIndices, ikModified);

                    AimIKParams params;
                    params.TargetBoneIndex = ikTarget.AimBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget.AimTarget, entityWorldTransform);
                    params.AimAxis = glm::normalize(ikTarget.AimAxis);
                    params.AimOffset = ikTarget.AimOffset;
                    params.PoleVector = glm::normalize(ikTarget.AimPoleVector);
                    params.ChainLength = aimChain;
                    params.ChainFactor = glm::clamp(ikTarget.AimChainFactor, 0.0f, 1.0f);
                    params.Weight = aimWeight;
                    AimIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
                }
            }
        }

        if (ikTarget.LimbIKEnabled && ikTarget.LimbBoneIndex < static_cast<u32>(boneCount))
        {
            if (isFiniteVec3(ikTarget.LimbTarget) && std::isfinite(ikTarget.LimbWeight))
            {
                // Skip if clamped weight is zero — no visible effect
                if (auto limbWeight = glm::clamp(ikTarget.LimbWeight, 0.0f, 1.0f); limbWeight > 0.0f)
                {
                    auto limbChain = std::clamp(ikTarget.LimbChainLength, 1u, static_cast<u32>(boneCount));
                    MarkIkChain(ikTarget.LimbBoneIndex, limbChain, boneCount, skeleton.m_ParentIndices, ikModified);

                    LimbIKParams params;
                    params.TargetBoneIndex = ikTarget.LimbBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget.LimbTarget, entityWorldTransform);
                    params.ChainLength = limbChain;
                    params.Weight = limbWeight;
                    LimbIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
                }
            }
        }

        // Only write back bones that IK actually modified
        for (sizet i = 0; i < boneCount; ++i)
        {
            if (ikModified[i])
            {
                skeleton.m_LocalTransforms[i] =
                    glm::translate(glm::mat4(1.0f), localPose[i].Translation) * glm::mat4_cast(localPose[i].Rotation) * glm::scale(glm::mat4(1.0f), localPose[i].Scale);
            }
        }
    }
} // namespace OloEngine::Animation
