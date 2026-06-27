#pragma once

#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

#include <vector>

namespace OloEngine
{

    class GameplayTagContainer
    {
      public:
        GameplayTagContainer() = default;

        void AddTag(const GameplayTag& tag);
        void RemoveTag(const GameplayTag& tag);
        void Clear();

        [[nodiscard("tag-presence check must be used")]] bool HasTagExact(const GameplayTag& tag) const;
        [[nodiscard("tag-presence check must be used")]] bool HasTagPartial(const GameplayTag& parent) const;
        [[nodiscard("tag-presence check must be used")]] bool HasAll(const GameplayTagContainer& required) const;
        [[nodiscard("tag-presence check must be used")]] bool HasAny(const GameplayTagContainer& tags) const;
        [[nodiscard("emptiness check must be used")]] bool IsEmpty() const
        {
            return m_Tags.empty();
        }
        [[nodiscard("tag count must be used")]] std::size_t Count() const
        {
            return m_Tags.size();
        }
        [[nodiscard("tag list must be used")]] const std::vector<GameplayTag>& GetTags() const
        {
            return m_Tags;
        }

        auto operator==(const GameplayTagContainer&) const -> bool = default;

      private:
        std::vector<GameplayTag> m_Tags;
    };

} // namespace OloEngine
