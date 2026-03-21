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

        [[nodiscard]] bool HasTagExact(const GameplayTag& tag) const;
        [[nodiscard]] bool HasTagPartial(const GameplayTag& parent) const;
        [[nodiscard]] bool HasAll(const GameplayTagContainer& required) const;
        [[nodiscard]] bool HasAny(const GameplayTagContainer& tags) const;
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Tags.empty();
        }
        [[nodiscard]] std::size_t Count() const
        {
            return m_Tags.size();
        }
        [[nodiscard]] const std::vector<GameplayTag>& GetTags() const
        {
            return m_Tags;
        }

        auto operator==(const GameplayTagContainer&) const -> bool = default;

      private:
        std::vector<GameplayTag> m_Tags;
    };

} // namespace OloEngine
