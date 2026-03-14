#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Bandwidth-aware priority queue for network entity updates.
    // Entities with higher relevance scores are sent first; the server
    // can cap how many entities are sent per frame to stay within bandwidth budget.
    class NetworkPriorityQueue
    {
      public:
        struct PriorityEntry
        {
            u64 EntityUUID = 0;
            f32 Score = 0.0f; // Higher = more important
        };

        NetworkPriorityQueue() = default;

        // Add or update an entity's priority score.
        // Score formula: ticksSinceLastUpdate / max(distanceSq, 1.0)
        // Staler + closer = higher priority.
        void UpdatePriority(u64 uuid, f32 distanceSq, u32 ticksSinceLastUpdate)
        {
            OLO_PROFILE_FUNCTION();

            f32 const dist = std::max(distanceSq, 1.0f);
            f32 const staleness = std::max(static_cast<f32>(ticksSinceLastUpdate), 1.0f);
            f32 const score = staleness / dist; // Staler + closer = higher priority

            // O(1) lookup via index map
            if (auto it = m_UUIDToIndex.find(uuid); it != m_UUIDToIndex.end())
            {
                m_Entries[it->second].Score = score;
                return;
            }
            m_UUIDToIndex[uuid] = static_cast<u32>(m_Entries.size());
            m_Entries.push_back({ uuid, score });
        }

        // Partially sort by priority (descending) and return top N entries.
        [[nodiscard]] std::vector<PriorityEntry> GetTopN(u32 count) const
        {
            OLO_PROFILE_FUNCTION();

            auto sorted = m_Entries;
            u32 const n = std::min(count, static_cast<u32>(sorted.size()));
            std::partial_sort(sorted.begin(), sorted.begin() + n, sorted.end(),
                              [](const PriorityEntry& a, const PriorityEntry& b)
                              { return a.Score > b.Score; });
            sorted.resize(n);
            return sorted;
        }

        // Remove an entity from the queue.
        void Remove(u64 uuid)
        {
            auto it = m_UUIDToIndex.find(uuid);
            if (it == m_UUIDToIndex.end())
            {
                return;
            }

            u32 const index = it->second;
            u32 const lastIndex = static_cast<u32>(m_Entries.size()) - 1;

            if (index != lastIndex)
            {
                // Swap with last element and update the swapped element's index
                u64 const lastUUID = m_Entries[lastIndex].EntityUUID;
                std::swap(m_Entries[index], m_Entries[lastIndex]);
                m_UUIDToIndex[lastUUID] = index;
            }

            m_Entries.pop_back();
            m_UUIDToIndex.erase(it);
        }

        // Clear all entries.
        void Clear()
        {
            m_Entries.clear();
            m_UUIDToIndex.clear();
        }

        // Get the number of entries.
        [[nodiscard]] u32 GetCount() const
        {
            return static_cast<u32>(m_Entries.size());
        }

      private:
        std::vector<PriorityEntry> m_Entries;
        std::unordered_map<u64, u32> m_UUIDToIndex; // uuid → index in m_Entries
    };
} // namespace OloEngine
