#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace OloEngine
{
    // A symbolic world-state for the GOAP planner.
    //
    // Facts are intentionally *discrete* — a bool or a 32-bit integer — so the
    // planning search space stays finite and every reachable state can be
    // hashed and compared cheaply. Continuous quantities must be discretised by
    // the game's sensors before they land here (e.g. expose ammo as the i32
    // "AmmoCount", or the derived bool "HasAmmo"). Floats are deliberately not a
    // fact type: A* would never converge over a continuous domain and equality
    // on floating-point is unsafe.
    //
    // The backing store is a vector kept sorted by key. A* copies a state for
    // every expanded edge, so a single-allocation copy plus O(n) equality and
    // hashing beats the per-node bucket churn of an unordered_map.
    class GoapWorldState
    {
      public:
        using Value = std::variant<bool, i32>;

        struct Fact
        {
            std::string Key;
            Value Val;

            auto operator==(const Fact& other) const -> bool = default;
        };

        GoapWorldState() = default;

        // Set (insert or overwrite) a fact.
        void Set(const std::string& key, Value value);
        void Set(const std::string& key, bool value)
        {
            Set(key, Value{ value });
        }
        void Set(const std::string& key, i32 value)
        {
            Set(key, Value{ value });
        }

        void Remove(const std::string& key);
        void Clear()
        {
            m_Facts.clear();
        }

        [[nodiscard]] bool Has(const std::string& key) const;
        [[nodiscard]] std::optional<Value> Get(const std::string& key) const;

        template<typename T>
        [[nodiscard]] T GetOr(const std::string& key, T fallback) const
        {
            if (auto it = Find(key); it != m_Facts.end())
            {
                if (auto* v = std::get_if<T>(&it->Val))
                    return *v;
            }
            return fallback;
        }

        // True when every fact in `conditions` is present here with an equal
        // value. Extra facts in this state are ignored. An empty condition set
        // is trivially satisfied.
        [[nodiscard]] bool Satisfies(const GoapWorldState& conditions) const;

        // Overlay every fact of `effects` onto this state (insert or overwrite).
        void ApplyEffects(const GoapWorldState& effects);

        // Count of facts in `conditions` not currently satisfied — the planner's
        // heuristic and a cheap progress metric for the agent.
        [[nodiscard]] u32 UnsatisfiedCount(const GoapWorldState& conditions) const;

        [[nodiscard]] const std::vector<Fact>& GetFacts() const
        {
            return m_Facts;
        }
        [[nodiscard]] bool Empty() const
        {
            return m_Facts.empty();
        }
        [[nodiscard]] sizet Size() const
        {
            return m_Facts.size();
        }

        // FNV-1a over the sorted (key, type-tag, value) triples. Used as the A*
        // closed-set key; full equality settles the (astronomically rare)
        // collision.
        [[nodiscard]] u64 Hash() const;

        auto operator==(const GoapWorldState& other) const -> bool
        {
            return m_Facts == other.m_Facts;
        }

      private:
        [[nodiscard]] std::vector<Fact>::const_iterator Find(const std::string& key) const;

        std::vector<Fact> m_Facts; // kept sorted by Key
    };
} // namespace OloEngine
