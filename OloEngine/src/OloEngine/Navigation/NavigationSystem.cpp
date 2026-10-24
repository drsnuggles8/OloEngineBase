#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavigationSystem.h"
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

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

            // A root-motion agent (issue #1559) is moved by its animation, so it
            // stays out of the crowd: DetourCrowd integrates its own velocity and
            // writing that position back would move the agent a second time. One
            // switched to root motion mid-play leaves the crowd here.
            if (agent.m_UseRootMotion && agent.m_CrowdAgentId >= 0 && crowdMgr && crowdMgr->IsValid())
            {
                crowdMgr->RemoveAgent(agent.m_CrowdAgentId);
                agent.m_CrowdAgentId = -1;
            }

            // If a crowd is running, every NavAgent participates in it (even one with
            // no target yet) so DetourCrowd's separation/avoidance accounts for it
            // against its neighbours. Register lazily on first tick rather than via
            // OnComponentAdded — that naturally handles "navmesh baked/set after the
            // component was added" ordering, and a SetNavMesh reset (which zeroes
            // every m_CrowdAgentId) re-registers everyone next frame for free.
            if (crowdMgr && crowdMgr->IsValid() && !agent.m_UseRootMotion)
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
                // NavMeshQuery exposes a std::vector output; the component owns native storage.
                std::vector<glm::vec3> pathCorners;
                const FindPathResult result =
                    navQuery->FindPath(transform.Translation, agent.m_TargetPosition, pathCorners);
                agent.m_PathCorners.Reset();
                agent.m_PathCorners.Append(pathCorners.data(), static_cast<i32>(pathCorners.size()));
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

            if (!agent.m_HasPath || agent.m_PathCorners.IsEmpty())
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
                if (agent.m_CurrentCornerIndex >= static_cast<u32>(agent.m_PathCorners.Num()))
                {
                    agent.m_HasPath = false;
                    agent.m_PathCorners.Reset();
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
            else if (agent.m_UseRootMotion)
            {
                // Root motion owns the translation (it ran earlier this tick:
                // RootMotionApply -> Navigation); navigation owns the heading.
                // Turn the agent's local +Z -- the glTF forward its clips travel
                // along -- toward the corner about world up, at most TurnSpeed.
                const glm::vec2 wanted(toTarget.x, toTarget.z);
                if (glm::dot(wanted, wanted) > EPSILON * EPSILON)
                {
                    const glm::vec3 forward = transform.GetRotation() * glm::vec3(0.0f, 0.0f, 1.0f);
                    const f32 currentYaw = std::atan2(forward.x, forward.z);
                    const f32 wantedYaw = std::atan2(wanted.x, wanted.y);
                    const f32 delta = std::remainder(wantedYaw - currentYaw, glm::two_pi<f32>());
                    const f32 maxTurn = std::max(agent.m_TurnSpeed, 0.0f) * dt;
                    const f32 turn = std::clamp(delta, -maxTurn, maxTurn);
                    if (std::isfinite(turn) && turn != 0.0f)
                    {
                        transform.SetRotation(glm::normalize(glm::angleAxis(turn, glm::vec3(0.0f, 1.0f, 0.0f)) * transform.GetRotation()));
                    }
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
