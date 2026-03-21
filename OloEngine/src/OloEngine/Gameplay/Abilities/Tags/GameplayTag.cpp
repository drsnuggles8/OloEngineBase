#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

namespace OloEngine
{

    GameplayTag::GameplayTag(std::string tagPath)
        : m_TagPath(std::move(tagPath))
    {
        m_Hash = static_cast<u32>(std::hash<std::string>{}(m_TagPath));
    }

    bool GameplayTag::MatchesExact(const GameplayTag& other) const
    {
        return m_Hash == other.m_Hash && m_TagPath == other.m_TagPath;
    }

    bool GameplayTag::MatchesPartial(const GameplayTag& parent) const
    {
        if (parent.m_TagPath.empty())
        {
            return true;
        }
        if (m_TagPath.size() < parent.m_TagPath.size())
        {
            return false;
        }
        // Check prefix match: must match exactly and either be the full tag or followed by '.'
        if (m_TagPath.compare(0, parent.m_TagPath.size(), parent.m_TagPath) != 0)
        {
            return false;
        }
        return m_TagPath.size() == parent.m_TagPath.size() || m_TagPath[parent.m_TagPath.size()] == '.';
    }

} // namespace OloEngine
