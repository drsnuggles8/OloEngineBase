#pragma once

#include "OloEngine/Core/Base.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;

    // Controls which entities are relevant to each client for snapshot replication.
    // Filters by distance (RelevanceRadius) and interest group membership.
    class NetworkInterestManager
    {
      public:
        NetworkInterestManager() = default;

        // Set the "observer" position for a given client (typically their controlled entity).
        void SetClientPosition(u32 clientID, const glm::vec3& position);

        // Set which interest groups a client is subscribed to.
        // Group 0 is the "default" group and entities in it are always included.
        void SetClientInterestGroups(u32 clientID, std::unordered_set<u32> groups);

        // Compute the set of entity UUIDs relevant to a given client.
        // An entity is relevant if:
        //   1. It has no NetworkInterestComponent (always relevant), OR
        //   2. Its RelevanceRadius is 0 (always relevant), OR
        //   3. The client is within RelevanceRadius AND the entity's InterestGroup
        //      matches one of the client's subscribed groups (or is group 0).
        [[nodiscard]] std::vector<u64> GetRelevantEntities(u32 clientID, Scene& scene) const;

        // Check if a specific entity is relevant to a client.
        [[nodiscard]] bool IsEntityRelevant(u32 clientID, u64 entityUUID, Scene& scene) const;

      private:
        std::unordered_map<u32, glm::vec3> m_ClientPositions;
        std::unordered_map<u32, std::unordered_set<u32>> m_ClientInterestGroups;
    };
} // namespace OloEngine
