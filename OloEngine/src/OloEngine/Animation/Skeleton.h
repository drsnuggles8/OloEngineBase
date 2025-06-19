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

        Skeleton() = default;
        Skeleton(size_t boneCount)
        {
            m_ParentIndices.resize(boneCount);
            m_BoneNames.resize(boneCount);
            m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
        }
    };
}
