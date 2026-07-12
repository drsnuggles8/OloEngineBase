#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavigationSystem.h"
#include "OloEngine/Navigation/CrowdManager.h"
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
        const auto* navQuery = scene->GetNavMeshQuery();

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

            // If a crowd is running, every NavAgent participates in it (even one with
            // no target yet) so DetourCrowd's separation/avoidance accounts for it
            // against its neighbours. Register lazily on first tick rather than via
            // OnComponentAdded — that naturally handles "navmesh baked/set after the
            // component was added" ordering, and a SetNavMesh reset (which zeroes
            // every m_CrowdAgentId) re-registers everyone next frame for free.
            if (crowdMgr && crowdMgr->IsValid())
            {
                if (agent.m_CrowdAgentId < 0)
                    agent.m_CrowdAgentId = crowdMgr->AddAgent(transform.Translation, agent);

                if (agent.m_CrowdAgentId >= 0)
                {
                    // Issue the move request once per target (mirrors the manual
                    // follower's "!m_HasPath && m_HasTarget" repath gate below —
                    // m_HasPath here means "request issued", not "corners computed").
                    if (agent.m_HasTarget && !agent.m_HasPath && !agent.m_TargetUnreachable)
                    {
                        if (crowdMgr->SetAgentTarget(agent.m_CrowdAgentId, agent.m_TargetPosition))
                        {
                            agent.m_HasPath = true;
                        }
                        else
                        {
                            // No navmesh polygon near the target at all — the crowd
                            // equivalent of FindPathResult::Failed.
                            agent.m_HasPath = false;
                            agent.m_TargetUnreachable = true;
                        }
                    }

                    if (glm::vec3 pos; crowdMgr->GetAgentPosition(agent.m_CrowdAgentId, pos))
                    {
                        if (agent.m_LockYAxis)
                            pos.y = transform.Translation.y;
                        transform.Translation = pos;
                    }

                    if (agent.m_HasTarget && agent.m_HasPath)
                    {
                        switch (crowdMgr->GetAgentTargetState(agent.m_CrowdAgentId))
                        {
                            case CrowdTargetState::Unreachable:
                                // Latch, same as the manual follower's partial-path case —
                                // the agent keeps walking toward the nearest reachable point.
                                agent.m_TargetUnreachable = true;
                                break;
                            case CrowdTargetState::Valid:
                            {
                                glm::vec3 toTarget = agent.m_TargetPosition - transform.Translation;
                                if (agent.m_LockYAxis)
                                    toTarget.y = 0.0f;
                                constexpr f32 EPSILON = 1e-4f;
                                if (glm::length(toTarget) < std::max(agent.m_StoppingDistance, EPSILON))
                                {
                                    agent.m_HasPath = false;
                                    agent.m_HasTarget = false;
                                }
                                break;
                            }
                            case CrowdTargetState::None:
                            case CrowdTargetState::Pending:
                                break;
                        }
                    }

                    continue;
                }
                // Crowd is full or the add otherwise failed — fall through to the
                // naive follower for this frame; m_CrowdAgentId stays -1 so the next
                // tick retries registration.
            }

            // Manual pathfinding for agents not in crowd. Once a target is flagged
            // unreachable we stop recomputing — otherwise a disconnected/off-navmesh
            // target would re-run FindPath every frame and (via the manual follower or
            // BTMoveTo) spin forever. The unreachable flag is the terminal signal.
            if (!agent.m_HasPath && agent.m_HasTarget && !agent.m_TargetUnreachable)
            {
                const FindPathResult result =
                    navQuery->FindPath(transform.Translation, agent.m_TargetPosition, agent.m_PathCorners);
                agent.m_CurrentCornerIndex = 0;

                if (result == FindPathResult::Failed)
                {
                    // No path at all. Latch unreachable but keep m_HasTarget set so
                    // consumers (BTMoveTo / scripts) can observe the terminal outcome.
                    agent.m_HasPath = false;
                    agent.m_TargetUnreachable = true;
                }
                else
                {
                    // Complete OR Partial: follow the corners we have. A partial path
                    // walks the agent to the nearest reachable point; the flag marks
                    // that it will never actually arrive at the requested target.
                    agent.m_HasPath = true;
                    agent.m_TargetUnreachable = (result == FindPathResult::Partial);
                }
            }

            if (!agent.m_HasPath || agent.m_PathCorners.empty())
                continue;

            // Move toward current corner (full 3D, or XZ-only when LockYAxis)
            const glm::vec3& target = agent.m_PathCorners[agent.m_CurrentCornerIndex];
            glm::vec3 toTarget = target - transform.Translation;
            if (agent.m_LockYAxis)
                toTarget.y = 0.0f;
            f32 dist = glm::length(toTarget);

            constexpr f32 EPSILON = 1e-4f;
            if (dist < std::max(agent.m_StoppingDistance, EPSILON))
            {
                ++agent.m_CurrentCornerIndex;
                if (agent.m_CurrentCornerIndex >= static_cast<u32>(agent.m_PathCorners.size()))
                {
                    agent.m_HasPath = false;
                    agent.m_PathCorners.clear();
                    agent.m_CurrentCornerIndex = 0;
                    // Only clear the target when we actually reached it. For a partial
                    // path we've walked to the nearest reachable point but the target is
                    // unreachable — keep m_HasTarget + m_TargetUnreachable set so the
                    // consumer sees a stable terminal state instead of a re-issue loop.
                    if (!agent.m_TargetUnreachable)
                        agent.m_HasTarget = false;
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
