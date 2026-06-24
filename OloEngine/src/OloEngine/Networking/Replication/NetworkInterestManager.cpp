#include "OloEnginePCH.h"
#include "NetworkInterestManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Debug/Profiler.h"

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // Above this many grid cells per axis the cubic cell walk in SpatialGrid::QueryRadius costs
        // more than a flat scene scan, so GetRelevantEntities falls back to the full walk. This caps
        // the worst case at kMaxGridCellsPerAxis^3 cell probes regardless of the widest relevance
        // radius, so one misconfigured entity with a huge radius can never make queries slower than
        // the original O(n) scan.
        constexpr u32 kMaxGridCellsPerAxis = 32;
    } // namespace

    void NetworkInterestManager::SetClientPosition(u32 clientID, const glm::vec3& position)
    {
        m_ClientPositions[clientID] = position;
    }

    void NetworkInterestManager::SetClientInterestGroups(u32 clientID, std::unordered_set<u32> groups)
    {
        m_ClientInterestGroups[clientID] = std::move(groups);
    }

    void NetworkInterestManager::RemoveClient(u32 clientID)
    {
        m_ClientPositions.erase(clientID);
        m_ClientInterestGroups.erase(clientID);
    }

    void NetworkInterestManager::UpdateSpatialGrid(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        m_SpatialGrid.Clear();
        m_AlwaysRelevant.clear();
        m_MaxRelevanceRadius = 0.0f;

        auto view = scene.GetAllEntitiesWith<IDComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };

            if (entity.HasComponent<NetworkIdentityComponent>())
            {
                auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
                if (!nic.IsReplicated)
                {
                    continue;
                }
            }

            u64 const uuid = static_cast<u64>(entity.GetUUID());

            // Partition the replicated entity: a finite, positive RelevanceRadius makes it distance-
            // filterable (lives in the spatial grid). Radius 0, a negative radius, or a non-finite
            // (corrupt) radius are not distance-filterable and go in the flat always-relevant list —
            // keeping NaN/inf out of the grid so it can never poison m_MaxRelevanceRadius (which would
            // force every query to fall back to the full scan). The query-time gate in
            // EvaluateEntityRelevance then rejects a corrupt-radius entity outright.
            f32 relevanceRadius = 0.0f;
            if (entity.HasComponent<NetworkInterestComponent>())
            {
                relevanceRadius = entity.GetComponent<NetworkInterestComponent>().RelevanceRadius;
            }

            if (std::isfinite(relevanceRadius) && relevanceRadius > 0.0f)
            {
                auto const& tc = entity.GetComponent<TransformComponent>();
                m_SpatialGrid.InsertOrUpdate(uuid, tc.Translation);
                m_MaxRelevanceRadius = std::max(m_MaxRelevanceRadius, relevanceRadius);
            }
            else
            {
                m_AlwaysRelevant.push_back(uuid);
            }
        }
    }

    const SpatialGrid& NetworkInterestManager::GetSpatialGrid() const
    {
        return m_SpatialGrid;
    }

    bool NetworkInterestManager::EvaluateEntityRelevance(Entity entity, const glm::vec3& clientPos,
                                                         const std::unordered_set<u32>* clientGroups) const
    {
        // The full-scan candidate set is GetAllEntitiesWith<IDComponent, TransformComponent>; an entity
        // whose TransformComponent was removed after the grid snapshotted its UUID must drop out here
        // (and we must not dereference the now-missing component below).
        if (!entity.HasComponent<TransformComponent>())
        {
            return false;
        }

        // Replication gate: an entity carrying a NetworkIdentityComponent is only relevant while it is
        // marked replicated. Re-checked live (not trusted from the grid snapshot) so a mid-tick toggle
        // to IsReplicated == false can't leak an entity the full scan would now exclude.
        if (entity.HasComponent<NetworkIdentityComponent>() &&
            !entity.GetComponent<NetworkIdentityComponent>().IsReplicated)
        {
            return false;
        }

        // No interest component → always relevant (no group, no distance cull).
        if (!entity.HasComponent<NetworkInterestComponent>())
        {
            return true;
        }

        auto const& interest = entity.GetComponent<NetworkInterestComponent>();

        // Interest-group filter (group 0 is the default group and is always included).
        if (interest.InterestGroup != 0)
        {
            if (!clientGroups || clientGroups->find(interest.InterestGroup) == clientGroups->end())
            {
                return false; // Client not subscribed to this group.
            }
        }

        // Distance-based relevance (radius 0 means "always relevant"). A non-finite radius is corrupt
        // input — RelevanceRadius is not isfinite-validated on scene/save-game load — so reject the
        // entity rather than letting NaN/inf bypass distance culling or read as always-relevant.
        f32 const radius = interest.RelevanceRadius;
        if (!std::isfinite(radius))
        {
            return false;
        }
        if (radius > 0.0f)
        {
            auto const& tc = entity.GetComponent<TransformComponent>();
            f32 const distSq = glm::distance2(clientPos, tc.Translation);
            // Reject a non-finite position (corrupt transform) rather than leaning on NaN-compare quirks.
            if (!std::isfinite(distSq) || distSq > radius * radius)
            {
                return false; // Too far away (or invalid position).
            }
        }

        return true;
    }

    std::vector<u64> NetworkInterestManager::GetRelevantEntities(u32 clientID, Scene& scene) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u64> result;

        // Get client observer position (default to origin if not set)
        glm::vec3 clientPos{ 0.0f };
        if (auto it = m_ClientPositions.find(clientID); it != m_ClientPositions.end())
        {
            clientPos = it->second;
        }

        // Get client interest groups
        const std::unordered_set<u32>* clientGroups = nullptr;
        if (auto it = m_ClientInterestGroups.find(clientID); it != m_ClientInterestGroups.end())
        {
            clientGroups = &it->second;
        }

        // The grid can accelerate the query only when it has been populated (UpdateSpatialGrid ran)
        // AND the widest relevance radius spans few enough cells that the cubic cell walk in
        // QueryRadius stays cheaper than a flat scene scan. Otherwise fall back to the full walk.
        const bool gridPopulated = m_SpatialGrid.GetEntityCount() > 0 || !m_AlwaysRelevant.empty();
        const f32 cellSize = m_SpatialGrid.GetCellSize();
        const f32 cellsPerAxis = (cellSize > 0.0f)
                                     ? (2.0f * m_MaxRelevanceRadius / cellSize + 1.0f)
                                     : std::numeric_limits<f32>::infinity();
        const bool gridCheaperThanScan = cellsPerAxis <= static_cast<f32>(kMaxGridCellsPerAxis);

        if (gridPopulated && gridCheaperThanScan)
        {
            // Always-relevant entities skip the distance cull, but the group filter inside
            // EvaluateEntityRelevance still applies — funnel each through the shared helper.
            for (u64 const uuid : m_AlwaysRelevant)
            {
                if (auto entity = scene.TryGetEntityWithUUID(UUID(uuid));
                    entity && EvaluateEntityRelevance(*entity, clientPos, clientGroups))
                {
                    result.push_back(uuid);
                }
            }

            // Distance-filterable candidates: a single grid fetch bounded by the widest radius, then
            // the exact per-entity rules. The two candidate sets are disjoint by construction (a
            // positive radius lands an entity in the grid, otherwise in m_AlwaysRelevant), so the
            // union never double-counts.
            auto const candidates = m_SpatialGrid.QueryRadius(clientPos, m_MaxRelevanceRadius);
            for (u64 const uuid : candidates)
            {
                if (auto entity = scene.TryGetEntityWithUUID(UUID(uuid));
                    entity && EvaluateEntityRelevance(*entity, clientPos, clientGroups))
                {
                    result.push_back(uuid);
                }
            }

            return result;
        }

        // Fallback: full scene scan (grid not populated yet, or radius too wide for the grid to help).
        // The shared gate applies the replication + interest/distance rules per entity.
        auto view = scene.GetAllEntitiesWith<IDComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };
            if (EvaluateEntityRelevance(entity, clientPos, clientGroups))
            {
                result.push_back(static_cast<u64>(entity.GetUUID()));
            }
        }

        return result;
    }

    bool NetworkInterestManager::IsEntityRelevant(u32 clientID, u64 entityUUID, Scene& scene) const
    {
        OLO_PROFILE_FUNCTION();

        // Get client observer position (default to origin if not set)
        glm::vec3 clientPos{ 0.0f };
        if (auto it = m_ClientPositions.find(clientID); it != m_ClientPositions.end())
        {
            clientPos = it->second;
        }

        // Get client interest groups
        const std::unordered_set<u32>* clientGroups = nullptr;
        if (auto it = m_ClientInterestGroups.find(clientID); it != m_ClientInterestGroups.end())
        {
            clientGroups = &it->second;
        }

        // Direct UUID lookup instead of scanning the whole scene to find one entity. The shared gate
        // re-checks transform presence, the replication flag, and the interest/distance rules, so this
        // matches GetRelevantEntities membership exactly (including for a stale-snapshot entity).
        auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID));
        if (!entityOpt)
        {
            return false; // Entity not in scene.
        }

        return EvaluateEntityRelevance(*entityOpt, clientPos, clientGroups);
    }
} // namespace OloEngine
