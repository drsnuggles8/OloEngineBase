#pragma once

#include <functional>
#include <string>

#include "OloEngine/Core/Base.h"

namespace OloEngine
{

    class GameplayTag
    {
      public:
        GameplayTag() = default;
        explicit GameplayTag(std::string tagPath);

        [[nodiscard("match result must be used")]] bool MatchesExact(const GameplayTag& other) const;
        [[nodiscard("match result must be used")]] bool MatchesPartial(const GameplayTag& parent) const;

        [[nodiscard("tag string must be used")]] const std::string& GetTagString() const
        {
            return m_TagPath;
        }
        [[nodiscard("tag hash must be used")]] u32 GetHash() const
        {
            return m_Hash;
        }
        [[nodiscard("validity check must be used")]] bool IsValid() const
        {
            return !m_TagPath.empty();
        }

        auto operator==(const GameplayTag& other) const -> bool
        {
            return m_Hash == other.m_Hash && m_TagPath == other.m_TagPath;
        }
        // operator!= is synthesized from operator== in C++20 (was cpp:S6186).
        auto operator<(const GameplayTag& other) const -> bool
        {
            return m_TagPath < other.m_TagPath;
        }

      private:
        std::string m_TagPath;
        u32 m_Hash = 0;
    };

} // namespace OloEngine

template<>
struct std::hash<OloEngine::GameplayTag>
{
    std::size_t operator()(const OloEngine::GameplayTag& tag) const noexcept
    {
        return static_cast<std::size_t>(tag.GetHash());
    }
};
