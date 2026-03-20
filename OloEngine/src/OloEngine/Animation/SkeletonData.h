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
        std::vector<glm::mat4> m_BindPoseMatrices;        // Original bind pose global transforms
        std::vector<glm::mat4> m_InverseBindPoses;        // Inverse bind pose matrices for skinning
        std::vector<glm::mat4> m_BindPoseLocalTransforms; // Original bind pose local transforms

        // Accumulated non-bone ancestor transforms per bone.
        // Between a bone and its parent bone (or scene root for root bones),
        // there may be non-bone nodes whose transforms are constant and not
        // affected by animation. This vector accumulates those transforms
        // so they can be applied when computing GlobalTransforms.
        std::vector<glm::mat4> m_BonePreTransforms;

        SkeletonData() = default;

        SkeletonData(sizet boneCount)
        {
            m_ParentIndices.resize(boneCount, -1); // Initialize with -1 to indicate root bones
            m_BoneNames.resize(boneCount);
            m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
            m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
            m_BonePreTransforms.resize(boneCount, glm::mat4(1.0f));
        }

        /**
         * @brief Initialize bind pose from current global transforms
         */
        void SetBindPose()
        {
            const sizet boneCount = m_GlobalTransforms.size();
            if (m_LocalTransforms.size() != boneCount ||
                m_BindPoseMatrices.size() != boneCount ||
                m_InverseBindPoses.size() != boneCount ||
                m_BindPoseLocalTransforms.size() != boneCount)
            {
                m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
                m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
                m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
                m_BindPoseLocalTransforms.resize(boneCount, glm::mat4(1.0f));
            }

            m_BindPoseLocalTransforms = m_LocalTransforms;
            for (sizet i = 0; i < boneCount; ++i)
            {
                m_BindPoseMatrices[i] = m_GlobalTransforms[i];
                m_InverseBindPoses[i] = glm::inverse(m_GlobalTransforms[i]);
            }
        }
    };
} // namespace OloEngine
