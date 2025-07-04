#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace OloEngine
{
    // Skeleton data for animated mesh skinning
    class Skeleton
    {
    public:
        std::vector<int> m_ParentIndices;
        std::vector<std::string> m_BoneNames;
        std::vector<glm::mat4> m_LocalTransforms;
        std::vector<glm::mat4> m_GlobalTransforms;
        std::vector<glm::mat4> m_FinalBoneMatrices;
        
        // Bind pose data (inverse bind pose matrices for proper skinning)
        std::vector<glm::mat4> m_BindPoseMatrices;      // Original bind pose global transforms
        std::vector<glm::mat4> m_InverseBindPoses;      // Inverse bind pose matrices for skinning

        Skeleton() = default;
        Skeleton(size_t boneCount)
        {
            m_ParentIndices.resize(boneCount);
            m_BoneNames.resize(boneCount);
            m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
            m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
        }
        
        // Initialize bind pose from current global transforms
        void SetBindPose()
        {
            for (size_t i = 0; i < m_GlobalTransforms.size(); ++i)
            {
                m_BindPoseMatrices[i] = m_GlobalTransforms[i];
                m_InverseBindPoses[i] = glm::inverse(m_GlobalTransforms[i]);
            }
        }
    };
}
