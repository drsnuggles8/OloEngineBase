#include "OloEngine/Animation/AnimationClip.h"

namespace OloEngine
{
    const BoneAnimation* AnimationClip::FindBoneAnimation(const std::string& boneName) const
    {
        // Initialize cache if not already done
        if (!m_CacheInitialized)
        {
            InitializeBoneCache();
        }

        auto it = m_BoneCache.find(boneName);
        return (it != m_BoneCache.end()) ? it->second : nullptr;
    }

    void AnimationClip::InitializeBoneCache() const
    {
        m_BoneCache.clear();
        for (const auto& anim : BoneAnimations)
        {
            m_BoneCache[anim.BoneName] = &anim;
        }
        m_CacheInitialized = true;
    }

    void AnimationClip::InvalidateBoneCache()
    {
        m_BoneCache.clear();
        m_CacheInitialized = false;
    }

    const std::unordered_map<std::string, std::vector<std::pair<f64, f32>>>& AnimationClip::GetMorphTracks() const
    {
        if (!m_MorphTrackCacheInitialized)
        {
            m_MorphTrackCache.clear();
            for (const auto& kf : MorphKeyframes)
            {
                m_MorphTrackCache[kf.TargetName].push_back({ kf.Time, kf.Weight });
            }
            for (auto& [name, keys] : m_MorphTrackCache)
            {
                std::sort(keys.begin(), keys.end(),
                          [](const auto& a, const auto& b)
                          { return a.first < b.first; });
            }
            m_MorphTrackCacheInitialized = true;
        }
        return m_MorphTrackCache;
    }

    void AnimationClip::InvalidateMorphTrackCache()
    {
        m_MorphTrackCache.clear();
        m_MorphTrackCacheInitialized = false;
    }
} // namespace OloEngine
