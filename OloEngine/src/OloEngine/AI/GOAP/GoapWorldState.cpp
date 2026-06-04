#include "OloEnginePCH.h"
#include "OloEngine/AI/GOAP/GoapWorldState.h"

#include <algorithm>

namespace OloEngine
{
    std::vector<GoapWorldState::Fact>::const_iterator GoapWorldState::Find(const std::string& key) const
    {
        auto it = std::lower_bound(m_Facts.begin(), m_Facts.end(), key,
                                   [](const Fact& f, const std::string& k)
                                   { return f.Key < k; });
        if (it != m_Facts.end() && it->Key == key)
            return it;
        return m_Facts.end();
    }

    void GoapWorldState::Set(const std::string& key, Value value)
    {
        auto it = std::lower_bound(m_Facts.begin(), m_Facts.end(), key,
                                   [](const Fact& f, const std::string& k)
                                   { return f.Key < k; });
        if (it != m_Facts.end() && it->Key == key)
            it->Val = std::move(value);
        else
            m_Facts.insert(it, Fact{ key, std::move(value) });
    }

    void GoapWorldState::Remove(const std::string& key)
    {
        auto it = std::lower_bound(m_Facts.begin(), m_Facts.end(), key,
                                   [](const Fact& f, const std::string& k)
                                   { return f.Key < k; });
        if (it != m_Facts.end() && it->Key == key)
            m_Facts.erase(it);
    }

    bool GoapWorldState::Has(const std::string& key) const
    {
        return Find(key) != m_Facts.end();
    }

    std::optional<GoapWorldState::Value> GoapWorldState::Get(const std::string& key) const
    {
        if (auto it = Find(key); it != m_Facts.end())
            return it->Val;
        return std::nullopt;
    }

    bool GoapWorldState::Satisfies(const GoapWorldState& conditions) const
    {
        // Both stores are sorted by key, so a merge-walk checks every condition
        // in one linear pass.
        auto cond = conditions.m_Facts.begin();
        auto self = m_Facts.begin();
        while (cond != conditions.m_Facts.end())
        {
            while (self != m_Facts.end() && self->Key < cond->Key)
                ++self;
            if (self == m_Facts.end() || self->Key != cond->Key || !(self->Val == cond->Val))
                return false;
            ++cond;
        }
        return true;
    }

    void GoapWorldState::ApplyEffects(const GoapWorldState& effects)
    {
        for (const auto& fact : effects.m_Facts)
            Set(fact.Key, fact.Val);
    }

    u32 GoapWorldState::UnsatisfiedCount(const GoapWorldState& conditions) const
    {
        u32 count = 0;
        auto cond = conditions.m_Facts.begin();
        auto self = m_Facts.begin();
        while (cond != conditions.m_Facts.end())
        {
            while (self != m_Facts.end() && self->Key < cond->Key)
                ++self;
            if (self == m_Facts.end() || self->Key != cond->Key || !(self->Val == cond->Val))
                ++count;
            ++cond;
        }
        return count;
    }

    u64 GoapWorldState::Hash() const
    {
        // FNV-1a 64-bit over the canonical, key-sorted fact list. The value's
        // active alternative index is folded in so bool(false) and i32(0) — which
        // would otherwise hash identically — stay distinct.
        constexpr u64 kOffsetBasis = 14695981039346656037ull;
        constexpr u64 kPrime = 1099511628211ull;

        u64 hash = kOffsetBasis;
        auto mix = [&hash](u64 byte)
        { hash = (hash ^ byte) * kPrime; };

        for (const auto& fact : m_Facts)
        {
            for (char c : fact.Key)
                mix(static_cast<u64>(static_cast<u8>(c)));
            mix(0xFFull); // key/value separator

            mix(static_cast<u64>(fact.Val.index()));
            std::visit([&mix](auto const& v)
                       {
                           using T = std::decay_t<decltype(v)>;
                           if constexpr (std::is_same_v<T, bool>)
                               mix(v ? 1ull : 0ull);
                           else // i32
                               mix(static_cast<u64>(static_cast<u32>(v))); },
                       fact.Val);
        }
        return hash;
    }
} // namespace OloEngine
