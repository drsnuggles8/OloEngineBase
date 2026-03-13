#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
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
        // Score formula: 1.0 / (distanceSq * ticksSinceLastUpdate)
        void UpdatePriority(u64 uuid, f32 distanceSq, u32 ticksSinceLastUpdate)
        {
            f32 const dist = std::max(distanceSq, 1.0f);
            f32 const staleness = std::max(static_cast<f32>(ticksSinceLastUpdate), 1.0f);
            f32 const score = staleness / dist; // Staler + closer = higher priority

            // Check if already in queue
            for (auto& entry : m_Entries)
            {
                if (entry.EntityUUID == uuid)
                {
                    entry.Score = score;
                    return;
                }
            }
            m_Entries.push_back({ uuid, score });
        }

        // Sort by priority (descending) and return top N entries.
        [[nodiscard]] std::vector<PriorityEntry> GetTopN(u32 count) const
        {
            auto sorted = m_Entries;
            std::sort(sorted.begin(), sorted.end(),
                      [](const PriorityEntry& a, const PriorityEntry& b)
                      { return a.Score > b.Score; });

            if (sorted.size() > count)
            {
                sorted.resize(count);
            }
            return sorted;
        }

        // Remove an entity from the queue.
        void Remove(u64 uuid)
        {
            std::erase_if(m_Entries, [uuid](const PriorityEntry& e)
                          { return e.EntityUUID == uuid; });
        }

        // Clear all entries.
        void Clear()
        {
            m_Entries.clear();
        }

        // Get the number of entries.
        [[nodiscard]] u32 GetCount() const
        {
            return static_cast<u32>(m_Entries.size());
        }

      private:
        std::vector<PriorityEntry> m_Entries;
    };
} // namespace OloEngine
