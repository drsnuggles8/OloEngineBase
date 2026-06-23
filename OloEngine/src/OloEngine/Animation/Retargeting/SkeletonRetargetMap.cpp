#include "OloEnginePCH.h"
#include "OloEngine/Animation/Retargeting/SkeletonRetargetMap.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace OloEngine::Animation
{
    std::string SkeletonRetargetMap::NormalizeBoneName(std::string_view name)
    {
        // Strip the namespace / rig prefix: keep only what follows the last ':' or '|'
        // ("mixamorig:Hips" -> "Hips", "Armature|LeftArm" -> "LeftArm").
        if (const sizet sep = name.find_last_of(":|"); sep != std::string_view::npos)
            name.remove_prefix(sep + 1);

        std::string out;
        out.reserve(name.size());
        for (const char c : name)
        {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0)
                out.push_back(static_cast<char>(std::tolower(uc)));
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
            normalized.try_emplace(NormalizeBoneName(source.m_BoneNames[s]), static_cast<int>(s));
        }

        for (sizet t = 0; t < target.m_BoneNames.size(); ++t)
        {
            if (const auto it = exact.find(target.m_BoneNames[t]); it != exact.end())
            {
                map.m_TargetToSource[t] = it->second;
                continue;
            }
            if (const auto it = normalized.find(NormalizeBoneName(target.m_BoneNames[t])); it != normalized.end())
                map.m_TargetToSource[t] = it->second;
        }

        return map;
    }

    void SkeletonRetargetMap::SetBoneMapping(int targetBoneIndex, int sourceBoneIndex)
    {
        if (targetBoneIndex < 0)
            return;
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
