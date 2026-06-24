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
        //   * distance-filterable (NetworkInterestComponent with a finite, positive RelevanceRadius) —
        //     inserted into the spatial grid so GetRelevantEntities() can pre-filter candidates by
        //     position;
        //   * always-relevant (no NetworkInterestComponent, RelevanceRadius <= 0, or a non-finite
        //     radius) — recorded in a flat list because no distance cull applies (a group filter, or
        //     the non-finite-radius rejection, still may exclude it at query time).
        // This is a SNAPSHOT used only for spatial candidate selection. GetRelevantEntities() still
        // re-checks transform presence, the replication flag, and the interest/distance rules against
        // the LIVE entity, so a transform removal or an IsReplicated toggle after this call cannot make
        // the grid path leak an entity the full scan would exclude. The snapshot's positions and
        // membership are trusted for candidate selection, though, so an entity that MOVED, became
        // replicated, gained a NetworkInterestComponent, or changed its radius bucket after this call
        // may be missed until the grid is rebuilt. Call once per tick before querying relevance.
        void UpdateSpatialGrid(Scene& scene);

        // Access the spatial grid (for testing/debugging). After UpdateSpatialGrid() this holds only
        // the distance-filterable replicated entities (see UpdateSpatialGrid above).
        [[nodiscard]] const SpatialGrid& GetSpatialGrid() const;

      private:
        // The single live relevance gate for one entity: transform presence, the replication flag, the
        // interest-group filter, and distance culling (a non-finite RelevanceRadius is rejected). Every
        // query path (grid-accelerated and full-scan) and IsEntityRelevant funnel through this against
        // the LIVE entity, so a stale grid snapshot (transform removed, IsReplicated toggled) can never
        // make them diverge. Dereferences no component it has not first guarded with HasComponent.
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
