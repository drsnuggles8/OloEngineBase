#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraphSystem.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/AimIKSolver.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Core/Log.h"
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine::Animation
{
    void AnimationGraphSystem::Update(
        AnimationGraphComponent& graphComp,
        Skeleton& skeleton,
        f32 deltaTime,
        const IKTargetComponent* ikTarget,
        const glm::mat4& entityWorldTransform)
    {
        OLO_PROFILE_FUNCTION();

        if (!graphComp.RuntimeGraph)
        {
            return;
        }

        sizet boneCount = skeleton.m_BoneNames.size();
        if (boneCount == 0)
        {
            return;
        }

        // Sync per-entity parameters into the runtime graph
        graphComp.RuntimeGraph->Parameters = graphComp.Parameters;

        // Evaluate the animation graph directly into the skeleton's local transform buffer
        graphComp.RuntimeGraph->Update(deltaTime, boneCount, skeleton.m_LocalTransforms, skeleton.m_BoneNames);

        // Copy parameters back (triggers may have been consumed)
        graphComp.Parameters = graphComp.RuntimeGraph->Parameters;

        // Ensure output vectors are sized before forward kinematics
        OLO_CORE_ASSERT(skeleton.m_ParentIndices.size() >= boneCount, "ParentIndices too small for boneCount");
        skeleton.m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
        skeleton.m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));

        // Apply IK pass between pose evaluation and forward kinematics
        if (ikTarget && (ikTarget->AimIKEnabled || ikTarget->LimbIKEnabled))
        {
            // Track which bones the IK chains will modify so we only write those
            // back — avoids the lossy glm::decompose round-trip on untouched bones.
            std::vector<bool> ikModified(boneCount, false);

            if (ikTarget->AimIKEnabled && ikTarget->AimBoneIndex < static_cast<u32>(boneCount))
            {
                auto bone = ikTarget->AimBoneIndex;
                for (u32 j = 0; j < std::max(1u, ikTarget->AimChainLength) && bone < static_cast<u32>(boneCount); ++j)
                {
                    ikModified[bone] = true;
                    i32 parent = skeleton.m_ParentIndices[bone];
                    if (parent < 0)
                    {
                        break;
                    }
                    bone = static_cast<u32>(parent);
                }
            }

            if (ikTarget->LimbIKEnabled && ikTarget->LimbBoneIndex < static_cast<u32>(boneCount))
            {
                auto bone = ikTarget->LimbBoneIndex;
                for (u32 j = 0; j < std::max(1u, ikTarget->LimbChainLength) && bone < static_cast<u32>(boneCount); ++j)
                {
                    ikModified[bone] = true;
                    i32 parent = skeleton.m_ParentIndices[bone];
                    if (parent < 0)
                    {
                        break;
                    }
                    bone = static_cast<u32>(parent);
                }
            }

            // Decompose mat4 local transforms to BoneTransform for IK solvers
            std::vector<BoneTransform> localPose(boneCount);
            for (sizet i = 0; i < boneCount; ++i)
            {
                glm::vec3 scale;
                glm::vec3 translation;
                glm::quat rotation;
                glm::vec3 skew;
                glm::vec4 perspective;
                glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective);
                localPose[i] = { translation, rotation, scale };
            }

            if (ikTarget->AimIKEnabled && ikTarget->AimBoneIndex < static_cast<u32>(boneCount))
            {
                auto isFiniteVec3 = [](const glm::vec3& v)
                { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
                if (isFiniteVec3(ikTarget->AimTarget) && isFiniteVec3(ikTarget->AimAxis) && isFiniteVec3(ikTarget->AimPoleVector) && std::isfinite(ikTarget->AimWeight) && std::isfinite(ikTarget->AimChainFactor))
                {
                    AimIKParams params;
                    params.TargetBoneIndex = ikTarget->AimBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget->AimTarget, entityWorldTransform);
                    params.AimAxis = ikTarget->AimAxis;
                    params.AimOffset = ikTarget->AimOffset;
                    params.PoleVector = ikTarget->AimPoleVector;
                    params.ChainLength = std::max(1u, ikTarget->AimChainLength);
                    params.ChainFactor = glm::clamp(ikTarget->AimChainFactor, 0.0f, 1.0f);
                    params.Weight = glm::clamp(ikTarget->AimWeight, 0.0f, 1.0f);
                    AimIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
                }
            }

            if (ikTarget->LimbIKEnabled && ikTarget->LimbBoneIndex < static_cast<u32>(boneCount))
            {
                auto isFiniteVec3 = [](const glm::vec3& v)
                { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
                if (isFiniteVec3(ikTarget->LimbTarget) && std::isfinite(ikTarget->LimbWeight))
                {
                    LimbIKParams params;
                    params.TargetBoneIndex = ikTarget->LimbBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget->LimbTarget, entityWorldTransform);
                    params.ChainLength = std::max(1u, ikTarget->LimbChainLength);
                    params.Weight = glm::clamp(ikTarget->LimbWeight, 0.0f, 1.0f);
                    LimbIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
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

        // Compute global transforms (forward kinematics)
        // Pre-transforms account for non-bone intermediate nodes in the hierarchy
        for (sizet i = 0; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            const glm::mat4& preTransform = (i < skeleton.m_BonePreTransforms.size())
                                                ? skeleton.m_BonePreTransforms[i]
                                                : glm::mat4(1.0f);
            i32 parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
            {
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[parent] * preTransform * skeleton.m_LocalTransforms[i];
            }
            else
            {
                skeleton.m_GlobalTransforms[i] = preTransform * skeleton.m_LocalTransforms[i];
            }
        }

        // Compute final bone matrices for GPU skinning (GlobalTransform * InverseBindPose)
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
        {
            if (i < skeleton.m_InverseBindPoses.size())
            {
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i] * skeleton.m_InverseBindPoses[i];
            }
            else
            {
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i];
            }
        }
    }
} // namespace OloEngine::Animation
