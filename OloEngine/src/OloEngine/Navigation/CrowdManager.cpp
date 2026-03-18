#include "OloEnginePCH.h"
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/Log.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>

namespace OloEngine
{
    CrowdManager::~CrowdManager()
    {
        Shutdown();
    }

    void CrowdManager::Initialize(const Ref<NavMesh>& navMesh, i32 maxAgents)
    {
        Shutdown();

        m_NavMesh = navMesh;
        if (!m_NavMesh || !m_NavMesh->GetDetourNavMesh())
        {
            OLO_CORE_ERROR("CrowdManager: Invalid navmesh");
            return;
        }

        m_Crowd = dtAllocCrowd();
        if (!m_Crowd)
        {
            OLO_CORE_ERROR("CrowdManager: Could not allocate crowd");
            return;
        }

        if (!m_Crowd->init(maxAgents, navMesh->GetSettings().AgentRadius * 4.0f, m_NavMesh->GetDetourNavMesh()))
        {
            OLO_CORE_ERROR("CrowdManager: Could not initialize crowd");
            dtFreeCrowd(m_Crowd);
            m_Crowd = nullptr;
        }
    }

    void CrowdManager::Shutdown()
    {
        if (m_Crowd)
        {
            dtFreeCrowd(m_Crowd);
            m_Crowd = nullptr;
        }
        m_NavMesh = nullptr;
    }

    i32 CrowdManager::AddAgent(const glm::vec3& position, const NavAgentComponent& agent)
    {
        if (!m_Crowd)
            return -1;

        dtCrowdAgentParams ap{};
        ap.radius = agent.m_Radius;
        ap.height = agent.m_Height;
        ap.maxAcceleration = agent.m_Acceleration;
        ap.maxSpeed = agent.m_MaxSpeed;
        ap.collisionQueryRange = ap.radius * 12.0f;
        ap.pathOptimizationRange = ap.radius * 30.0f;
        ap.separationWeight = 2.0f;
        ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        ap.obstacleAvoidanceType = 3;
        ap.queryFilterType = 0;

        const f32 pos[3] = { position.x, position.y, position.z };
        return m_Crowd->addAgent(pos, &ap);
    }

    void CrowdManager::RemoveAgent(i32 agentId)
    {
        if (m_Crowd && agentId >= 0)
            m_Crowd->removeAgent(agentId);
    }

    void CrowdManager::SetAgentTarget(i32 agentId, const glm::vec3& target)
    {
        if (!m_Crowd || agentId < 0)
            return;

        const f32 targetPos[3] = { target.x, target.y, target.z };
        const f32 ext[3] = { 2.0f, 4.0f, 2.0f };

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        const dtNavMeshQuery* query = m_Crowd->getNavMeshQuery();
        dtPolyRef targetRef = 0;
        f32 nearest[3]{};
        query->findNearestPoly(targetPos, ext, &filter, &targetRef, nearest);

        if (targetRef)
            m_Crowd->requestMoveTarget(agentId, targetRef, nearest);
    }

    void CrowdManager::Update(f32 dt)
    {
        if (m_Crowd)
            m_Crowd->update(dt, nullptr);
    }

    glm::vec3 CrowdManager::GetAgentPosition(i32 agentId) const
    {
        if (!m_Crowd || agentId < 0)
            return glm::vec3(0.0f);

        const dtCrowdAgent* agent = m_Crowd->getAgent(agentId);
        if (!agent || !agent->active)
            return glm::vec3(0.0f);

        return { agent->npos[0], agent->npos[1], agent->npos[2] };
    }

    glm::vec3 CrowdManager::GetAgentVelocity(i32 agentId) const
    {
        if (!m_Crowd || agentId < 0)
            return glm::vec3(0.0f);

        const dtCrowdAgent* agent = m_Crowd->getAgent(agentId);
        if (!agent || !agent->active)
            return glm::vec3(0.0f);

        return { agent->vel[0], agent->vel[1], agent->vel[2] };
    }

    i32 CrowdManager::GetActiveAgentCount() const
    {
        if (!m_Crowd)
            return 0;
        return m_Crowd->getAgentCount();
    }
} // namespace OloEngine
