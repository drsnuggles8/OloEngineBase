#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

namespace OloEngine
{

    void GameplayTagContainer::AddTag(const GameplayTag& tag)
    {
        if (!HasTagExact(tag))
        {
            m_Tags.push_back(tag);
        }
    }

    void GameplayTagContainer::RemoveTag(const GameplayTag& tag)
    {
        std::erase_if(m_Tags, [&tag](const GameplayTag& t) { return t.MatchesExact(tag); });
    }

    void GameplayTagContainer::Clear()
    {
        m_Tags.clear();
    }

    bool GameplayTagContainer::HasTagExact(const GameplayTag& tag) const
    {
        return std::ranges::any_of(m_Tags, [&tag](const GameplayTag& t) { return t.MatchesExact(tag); });
    }

    bool GameplayTagContainer::HasTagPartial(const GameplayTag& parent) const
    {
        return std::ranges::any_of(m_Tags, [&parent](const GameplayTag& t) { return t.MatchesPartial(parent); });
    }

    bool GameplayTagContainer::HasAll(const GameplayTagContainer& required) const
    {
        return std::ranges::all_of(required.m_Tags, [this](const GameplayTag& t) { return HasTagExact(t); });
    }

    bool GameplayTagContainer::HasAny(const GameplayTagContainer& tags) const
    {
        return std::ranges::any_of(tags.m_Tags, [this](const GameplayTag& t) { return HasTagExact(t); });
    }

} // namespace OloEngine
