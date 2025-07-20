#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Shared skeleton data structure for bone hierarchy and transforms
     * 
     * This structure contains the common data used by both Skeleton and SkeletonComponent,
     * eliminating duplication and centralizing skeleton layout management.
     */
    struct SkeletonData
    {
        // Bone hierarchy (indices, parent indices, names)
        std::vector<int> m_ParentIndices;
        std::vector<std::string> m_BoneNames;
        
        // Local and global transforms for each bone
        std::vector<glm::mat4> m_LocalTransforms;
        std::vector<glm::mat4> m_GlobalTransforms;
        
        // Final matrices for skinning (to be sent to GPU)
        std::vector<glm::mat4> m_FinalBoneMatrices;
        
        // Bind pose data for proper skinning
        std::vector<glm::mat4> m_BindPoseMatrices;      // Original bind pose global transforms
        std::vector<glm::mat4> m_InverseBindPoses;      // Inverse bind pose matrices for skinning

        SkeletonData() = default;
        
        SkeletonData(size_t boneCount)
        {
            m_ParentIndices.resize(boneCount, -1); // Initialize with -1 to indicate root bones
            m_BoneNames.resize(boneCount);
            m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
            m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
        }
        
        /**
         * @brief Initialize bind pose from current global transforms
         */
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
