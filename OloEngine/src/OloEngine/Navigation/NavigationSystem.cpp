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

        // Update crowd first so agents get current-frame positions
        if (crowdMgr && crowdMgr->IsValid())
            crowdMgr->Update(dt);

        auto view = scene->GetAllEntitiesWith<NavAgentComponent, TransformComponent>();

        for (auto e : view)
        {
            Entity entity = { e, scene };
            auto& agent = entity.GetComponent<NavAgentComponent>();
            auto& transform = entity.GetComponent<TransformComponent>();

            // If using crowd manager, sync from crowd
            if (crowdMgr && crowdMgr->IsValid() && agent.m_CrowdAgentId >= 0)
            {
                glm::vec3 pos;
                if (crowdMgr->GetAgentPosition(agent.m_CrowdAgentId, pos))
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

            // Move toward current corner (full 3D)
            const glm::vec3& target = agent.m_PathCorners[agent.m_CurrentCornerIndex];
            glm::vec3 toTarget = target - transform.Translation;
            f32 dist = glm::length(toTarget);

            constexpr f32 EPSILON = 1e-4f;
            if (dist < std::max(agent.m_StoppingDistance, EPSILON))
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
    }
} // namespace OloEngine
