#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Represents separate keyframe channels for efficient storage
    struct BonePositionKey
    {
        f64 Time;
        glm::vec3 Position;
    };

    struct BoneRotationKey
    {
        f64 Time;
        glm::quat Rotation;
    };

    struct BoneScaleKey
    {
        f64 Time;
        glm::vec3 Scale;
    };

    // Animation data for a single bone - stores original keyframes separately
    struct BoneAnimation
    {
        std::string BoneName;
        std::vector<BonePositionKey> PositionKeys;
        std::vector<BoneRotationKey> RotationKeys;
        std::vector<BoneScaleKey> ScaleKeys;
    };

    // AnimationClip: a set of bone animations and duration
    class AnimationClip
    {
    public:
        std::string Name;
        float Duration = 0.0f; // In seconds
        
        /**
         * @brief Vector of bone animations. 
         * @warning After modifying this vector (adding, removing, or changing elements),
         * you must call InvalidateBoneCache() to maintain cache validity.
         */
        std::vector<BoneAnimation> BoneAnimations;

        // Finds the animation for a given bone name
        const BoneAnimation* FindBoneAnimation(const std::string& boneName) const;
        
        // Initialize the bone lookup cache for performance
        void InitializeBoneCache() const;
        
        /**
         * @brief Invalidate the bone lookup cache - must be called after modifying BoneAnimations
         * 
         * The internal cache stores pointers to BoneAnimation elements. Any modification
         * to the BoneAnimations vector (resize, push_back, erase, etc.) can invalidate
         * these pointers, leading to undefined behavior. Call this method after any
         * structural changes to BoneAnimations to ensure cache safety.
         */
        void InvalidateBoneCache();
        
    private:
        // Cache for O(1) bone animation lookups
        mutable std::unordered_map<std::string, const BoneAnimation*> m_BoneCache;
        mutable bool m_CacheInitialized = false;
    };
}
