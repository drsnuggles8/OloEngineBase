#include "OloEnginePCH.h"
#include "OloEngine/Animation/Retargeting/SkeletonRetargetMap.h"
#include "OloEngine/Animation/Retargeting/HumanoidBoneMap.h"

#include <algorithm>
#include <locale>
#include <unordered_map>

namespace OloEngine::Animation
{
    std::string SkeletonRetargetMap::NormalizeBoneName(std::string_view name)
    {
        // Strip the namespace / rig prefix: keep only what follows the last ':' or '|'
        // ("mixamorig:Hips" -> "Hips", "Armature|LeftArm" -> "LeftArm").
        if (const sizet sep = name.find_last_of(":|"); sep != std::string_view::npos)
            name.remove_prefix(sep + 1);

        // Use the locale-aware <locale> overloads (MISRA C++ 24.5.1 forbids the
        // <cctype> character functions). The classic "C" locale keeps ASCII-only
        // classification/case-folding, matching the previous behaviour.
        const std::locale& loc = std::locale::classic();
        std::string out;
        out.reserve(name.size());
        for (const char c : name)
        {
            if (std::isalnum(c, loc))
                out.push_back(std::tolower(c, loc));
        }
        return out;
    }

    SkeletonRetargetMap SkeletonRetargetMap::BuildByName(const SkeletonData& source, const SkeletonData& target)
    {
        SkeletonRetargetMap map;
        map.m_TargetToSource.assign(target.m_BoneNames.size(), kUnmapped);

        // Index source bones by exact and normalized name. First occurrence wins so
        // a duplicate bone name (rare, but possible in malformed rigs) is stable.
        std::unordered_map<std::string, int> exact;
        std::unordered_map<std::string, int> normalized;
        exact.reserve(source.m_BoneNames.size());
        normalized.reserve(source.m_BoneNames.size());
        for (sizet s = 0; s < source.m_BoneNames.size(); ++s)
        {
            exact.try_emplace(source.m_BoneNames[s], static_cast<int>(s));
            // A name that is only a namespace/rig prefix or separators normalizes to
            // "" — don't index it, or every such name would collide on one bucket and
            // produce false-positive matches between unrelated bones.
            if (std::string norm = NormalizeBoneName(source.m_BoneNames[s]); !norm.empty())
                normalized.try_emplace(std::move(norm), static_cast<int>(s));
        }

        for (sizet t = 0; t < target.m_BoneNames.size(); ++t)
        {
            if (const auto it = exact.find(target.m_BoneNames[t]); it != exact.end())
            {
                map.m_TargetToSource[t] = it->second;
                continue;
            }
            if (const std::string norm = NormalizeBoneName(target.m_BoneNames[t]); !norm.empty())
            {
                if (const auto it = normalized.find(norm); it != normalized.end())
                    map.m_TargetToSource[t] = it->second;
            }
        }

        return map;
    }

    SkeletonRetargetMap SkeletonRetargetMap::BuildByHumanoidRole(const SkeletonData& source,
                                                                 const SkeletonData& target,
                                                                 const HumanoidBoneMap& sourceRoles,
                                                                 const HumanoidBoneMap& targetRoles)
    {
        SkeletonRetargetMap map;
        map.m_TargetToSource.assign(target.m_BoneNames.size(), kUnmapped);

        for (sizet r = 0; r < HumanoidBoneCount; ++r)
        {
            const auto role = static_cast<HumanoidBone>(r);
            const int sourceBone = sourceRoles.GetBone(role);
            const int targetBone = targetRoles.GetBone(role);
            if (sourceBone == HumanoidBoneMap::kUnassigned || targetBone == HumanoidBoneMap::kUnassigned)
                continue;

            // Defensive: a role map could reference a bone outside the skeleton it was
            // (or wasn't) built from. Skip rather than write out of bounds / record a
            // mapping the retargeter would have to re-validate anyway.
            if (targetBone < 0 || static_cast<sizet>(targetBone) >= map.m_TargetToSource.size())
                continue;
            if (sourceBone < 0 || static_cast<sizet>(sourceBone) >= source.m_BoneNames.size())
                continue;

            map.m_TargetToSource[static_cast<sizet>(targetBone)] = sourceBone;
        }

        return map;
    }

    SkeletonRetargetMap SkeletonRetargetMap::BuildByHumanoidRole(const SkeletonData& source, const SkeletonData& target)
    {
        return BuildByHumanoidRole(source, target, HumanoidBoneMap::AutoDetect(source),
                                   HumanoidBoneMap::AutoDetect(target));
    }

    void SkeletonRetargetMap::FillUnmappedFrom(const SkeletonRetargetMap& fallback)
    {
        const sizet count = std::min(m_TargetToSource.size(), fallback.m_TargetToSource.size());
        for (sizet t = 0; t < count; ++t)
        {
            if (m_TargetToSource[t] == kUnmapped)
                m_TargetToSource[t] = fallback.m_TargetToSource[t];
        }
    }

    void SkeletonRetargetMap::SetBoneMapping(int targetBoneIndex, int sourceBoneIndex)
    {
        if (targetBoneIndex < 0)
            return;
        // Normalize any invalid negative (e.g. caller passing -5, or kUnmapped to
        // clear) to kUnmapped, so HasMapping / GetMappedBoneCount stay consistent
        // (they treat kUnmapped as "no mapping").
        if (sourceBoneIndex < 0)
            sourceBoneIndex = kUnmapped;
        if (static_cast<sizet>(targetBoneIndex) >= m_TargetToSource.size())
            m_TargetToSource.resize(static_cast<sizet>(targetBoneIndex) + 1, kUnmapped);
        m_TargetToSource[static_cast<sizet>(targetBoneIndex)] = sourceBoneIndex;
    }

    int SkeletonRetargetMap::GetSourceBone(int targetBoneIndex) const
    {
        if (targetBoneIndex < 0 || static_cast<sizet>(targetBoneIndex) >= m_TargetToSource.size())
            return kUnmapped;
        return m_TargetToSource[static_cast<sizet>(targetBoneIndex)];
    }

    bool SkeletonRetargetMap::HasMapping(int targetBoneIndex) const
    {
        return GetSourceBone(targetBoneIndex) != kUnmapped;
    }

    sizet SkeletonRetargetMap::GetMappedBoneCount() const
    {
        return static_cast<sizet>(std::ranges::count_if(m_TargetToSource,
                                                        [](int s)
                                                        { return s != kUnmapped; }));
    }
} // namespace OloEngine::Animation
