#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/SpatialGrid.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;
    class Entity;

    // Controls which entities are relevant to each client for snapshot replication.
    // Uses a SpatialGrid internally for efficient spatial queries.
    class NetworkInterestManager
    {
      public:
        NetworkInterestManager() = default;

        // Set the "observer" position for a given client (typically their controlled entity).
        void SetClientPosition(u32 clientID, const glm::vec3& position);

        // Set which interest groups a client is subscribed to.
        // Group 0 is the "default" group and entities in it are always included.
        void SetClientInterestGroups(u32 clientID, std::unordered_set<u32> groups);

        // Remove a client from the interest manager (call on disconnect).
        void RemoveClient(u32 clientID);

        // Compute the set of entity UUIDs relevant to a given client.
        // An entity is relevant if:
        //   1. It has no NetworkInterestComponent (always relevant), OR
        //   2. Its RelevanceRadius is 0 (always relevant), OR
        //   3. The client is within RelevanceRadius AND the entity's InterestGroup
        //      matches one of the client's subscribed groups (or is group 0).
        [[nodiscard]] std::vector<u64> GetRelevantEntities(u32 clientID, Scene& scene) const;

        // Check if a specific entity is relevant to a client.
        [[nodiscard]] bool IsEntityRelevant(u32 clientID, u64 entityUUID, Scene& scene) const;

        // Rebuild the spatial acceleration structure from the scene's replicated entities.
        // Replicated entities are partitioned into two sets:
        //   * distance-filterable (NetworkInterestComponent with RelevanceRadius > 0) — inserted
        //     into the spatial grid so GetRelevantEntities() can pre-filter candidates by position;
        //   * always-relevant (no NetworkInterestComponent, or RelevanceRadius == 0) — recorded in a
        //     flat list because no distance cull applies (a group filter still may).
        // Should be called once per tick before querying relevance; the query paths assume the
        // grid's snapshot matches live transforms (no entity moved since this call).
        void UpdateSpatialGrid(Scene& scene);

        // Access the spatial grid (for testing/debugging). After UpdateSpatialGrid() this holds only
        // the distance-filterable replicated entities (see UpdateSpatialGrid above).
        [[nodiscard]] const SpatialGrid& GetSpatialGrid() const;

      private:
        // Apply the per-entity relevance rules (interest group + distance) to a single entity.
        // Precondition: entity has IDComponent + TransformComponent and, if it carries a
        // NetworkIdentityComponent, IsReplicated is true (callers enforce the replication gate).
        // Both query paths funnel through this so the grid-accelerated and full-scan results stay
        // bit-for-bit identical.
        [[nodiscard]] bool EvaluateEntityRelevance(Entity entity, const glm::vec3& clientPos,
                                                   const std::unordered_set<u32>* clientGroups) const;

        std::unordered_map<u32, glm::vec3> m_ClientPositions;
        std::unordered_map<u32, std::unordered_set<u32>> m_ClientInterestGroups;
        SpatialGrid m_SpatialGrid{ 64.0f };

        // Replicated entities that bypass distance culling (always relevant, subject only to the
        // group filter). Populated by UpdateSpatialGrid alongside m_SpatialGrid.
        std::vector<u64> m_AlwaysRelevant;

        // Largest RelevanceRadius among the grid's distance-filterable entities. Bounds the single
        // QueryRadius() candidate fetch in GetRelevantEntities (each candidate is then re-tested
        // against its own radius).
        f32 m_MaxRelevanceRadius = 0.0f;
    };
} // namespace OloEngine
