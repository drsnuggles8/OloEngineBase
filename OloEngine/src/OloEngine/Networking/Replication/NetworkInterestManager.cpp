#include "OloEnginePCH.h"
#include "NetworkInterestManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Debug/Profiler.h"

#include <glm/gtx/norm.hpp>

namespace OloEngine
{
    void NetworkInterestManager::SetClientPosition(u32 clientID, const glm::vec3& position)
    {
        m_ClientPositions[clientID] = position;
    }

    void NetworkInterestManager::SetClientInterestGroups(u32 clientID, std::unordered_set<u32> groups)
    {
        m_ClientInterestGroups[clientID] = std::move(groups);
    }

    void NetworkInterestManager::UpdateSpatialGrid(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        m_SpatialGrid.Clear();
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
            auto const& tc = entity.GetComponent<TransformComponent>();
            m_SpatialGrid.InsertOrUpdate(uuid, tc.Translation);
        }
    }

    const SpatialGrid& NetworkInterestManager::GetSpatialGrid() const
    {
        return m_SpatialGrid;
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

        auto view = scene.GetAllEntitiesWith<IDComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };
            u64 const uuid = static_cast<u64>(entity.GetUUID());

            // Only replicate entities with NetworkIdentityComponent that are marked as replicated
            if (entity.HasComponent<NetworkIdentityComponent>())
            {
                auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
                if (!nic.IsReplicated)
                {
                    continue;
                }
            }

            // Check interest filtering
            if (entity.HasComponent<NetworkInterestComponent>())
            {
                auto const& interest = entity.GetComponent<NetworkInterestComponent>();

                // Check interest group
                if (interest.InterestGroup != 0)
                {
                    if (!clientGroups || clientGroups->find(interest.InterestGroup) == clientGroups->end())
                    {
                        continue; // Client not subscribed to this group
                    }
                }

                // Use SpatialGrid for distance check when grid is populated, else fall back to direct check
                if (interest.RelevanceRadius > 0.0f)
                {
                    if (m_SpatialGrid.GetEntityCount() > 0)
                    {
                        auto nearby = m_SpatialGrid.QueryRadius(clientPos, interest.RelevanceRadius);
                        if (std::find(nearby.begin(), nearby.end(), uuid) == nearby.end())
                        {
                            continue; // Too far away
                        }
                    }
                    else
                    {
                        auto const& tc = entity.GetComponent<TransformComponent>();
                        f32 const distSq = glm::distance2(clientPos, tc.Translation);
                        if (distSq > interest.RelevanceRadius * interest.RelevanceRadius)
                        {
                            continue; // Too far away
                        }
                    }
                }
            }

            result.push_back(uuid);
        }

        return result;
    }

    bool NetworkInterestManager::IsEntityRelevant(u32 clientID, u64 entityUUID, Scene& scene) const
    {
        auto relevant = GetRelevantEntities(clientID, scene);
        return std::find(relevant.begin(), relevant.end(), entityUUID) != relevant.end();
    }
} // namespace OloEngine
