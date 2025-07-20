#include "OloEngine/Animation/AnimationClip.h"

namespace OloEngine
{
    const BoneAnimation* AnimationClip::FindBoneAnimation(const std::string& boneName) const
    {
        // Initialize cache if not already done
        if (!m_CacheInitialized)
        {
            const_cast<AnimationClip*>(this)->InitializeBoneCache();
        }
        
        auto it = m_BoneCache.find(boneName);
        return (it != m_BoneCache.end()) ? it->second : nullptr;
    }
    
    void AnimationClip::InitializeBoneCache()
    {
        m_BoneCache.clear();
        for (const auto& anim : BoneAnimations)
        {
            m_BoneCache[anim.BoneName] = &anim;
        }
        m_CacheInitialized = true;
    }
}
