#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Represents a single keyframe for a bone
    struct BoneKeyframe
    {
        float Time; // Time in seconds
        glm::vec3 Translation;
        glm::quat Rotation;
        glm::vec3 Scale;
    };

    // Animation data for a single bone
    struct BoneAnimation
    {
        std::string BoneName;
        std::vector<BoneKeyframe> Keyframes;
    };

    // AnimationClip: a set of bone animations and duration
    class AnimationClip
    {
    public:
        std::string Name;
        float Duration = 0.0f; // In seconds
        std::vector<BoneAnimation> BoneAnimations;

        // Finds the animation for a given bone name
        const BoneAnimation* FindBoneAnimation(const std::string& boneName) const;
        
        // Initialize the bone lookup cache for performance
        void InitializeBoneCache();
        
    private:
        // Cache for O(1) bone animation lookups
        mutable std::unordered_map<std::string, const BoneAnimation*> m_BoneCache;
        mutable bool m_CacheInitialized = false;
    };
}
