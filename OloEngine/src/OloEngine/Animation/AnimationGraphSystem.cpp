#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraphSystem.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine::Animation
{
    void AnimationGraphSystem::Update(
        AnimationGraphComponent& graphComp,
        Skeleton& skeleton,
        f32 deltaTime)
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

        // Evaluate the animation graph to produce local bone matrices
        std::vector<glm::mat4> localBoneMatrices;
        graphComp.RuntimeGraph->Update(deltaTime, boneCount, localBoneMatrices, skeleton.m_BoneNames);

        // Copy parameters back (triggers may have been consumed)
        graphComp.Parameters = graphComp.RuntimeGraph->Parameters;

        // Write local transforms to skeleton
        for (sizet i = 0; i < boneCount && i < localBoneMatrices.size(); ++i)
        {
            skeleton.m_LocalTransforms[i] = localBoneMatrices[i];
        }

        // Compute global transforms (forward kinematics)
        for (sizet i = 0; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            i32 parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
            {
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[parent] * skeleton.m_LocalTransforms[i];
            }
            else
            {
                skeleton.m_GlobalTransforms[i] = skeleton.m_LocalTransforms[i];
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
