#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavigationSystem.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    void NavigationSystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        auto* crowdMgr = scene->GetCrowdManager();
        auto* navQuery = scene->GetNavMeshQuery();

        if (!navQuery || !navQuery->IsValid())
            return;

        auto view = scene->GetAllEntitiesWith<NavAgentComponent, TransformComponent>();

        for (auto e : view)
        {
            Entity entity = { e, scene };
            auto& agent = entity.GetComponent<NavAgentComponent>();
            auto& transform = entity.GetComponent<TransformComponent>();

            // If using crowd manager, sync from crowd
            if (crowdMgr && crowdMgr->IsValid() && agent.m_CrowdAgentId >= 0)
            {
                glm::vec3 pos = crowdMgr->GetAgentPosition(agent.m_CrowdAgentId);
                if (pos != glm::vec3(0.0f))
                    transform.Translation = pos;
                continue;
            }

            // Manual pathfinding for agents not in crowd
            if (!agent.m_HasPath && agent.m_HasTarget)
            {
                agent.m_HasPath = navQuery->FindPath(transform.Translation, agent.m_TargetPosition, agent.m_PathCorners);
                agent.m_CurrentCornerIndex = 0;
                if (!agent.m_HasPath)
                    agent.m_HasTarget = false;
            }

            if (!agent.m_HasPath || agent.m_PathCorners.empty())
                continue;

            // Move toward current corner
            const glm::vec3& target = agent.m_PathCorners[agent.m_CurrentCornerIndex];
            glm::vec3 toTarget = target - transform.Translation;
            toTarget.y = 0.0f; // Keep movement on XZ plane
            f32 dist = glm::length(toTarget);

            if (dist < agent.m_StoppingDistance)
            {
                agent.m_CurrentCornerIndex++;
                if (agent.m_CurrentCornerIndex >= static_cast<u32>(agent.m_PathCorners.size()))
                {
                    agent.m_HasPath = false;
                    agent.m_HasTarget = false;
                    agent.m_PathCorners.clear();
                    continue;
                }
            }
            else
            {
                glm::vec3 direction = toTarget / dist;
                f32 speed = std::min(agent.m_MaxSpeed * dt, dist);
                transform.Translation += direction * speed;
            }
        }

        // Update crowd if active
        if (crowdMgr && crowdMgr->IsValid())
            crowdMgr->Update(dt);
    }
} // namespace OloEngine
