#include "OloEnginePCH.h"
#include "PerceptionSystem.h"
#include "PerceptionMath.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // Mirror the perceiver's current sensor result into one AI blackboard so
        // BT / FSM / GOAP / scripts can read PerceptionKeys::*.
        void WriteToBlackboard(BTBlackboard& bb, const PerceptionComponent& pc)
        {
            bb.Set(PerceptionKeys::CanSeeTarget, pc.HasVisibleTarget);

            if (pc.HasVisibleTarget)
            {
                bb.Set(PerceptionKeys::Target, pc.VisibleTarget);
            }
            else
            {
                bb.Remove(PerceptionKeys::Target);
            }

            if (pc.HasLastKnownPosition)
            {
                bb.Set(PerceptionKeys::LastKnownPosition, pc.LastKnownPosition);
            }
        }

        // Publish into whichever AI blackboards the perceiver carries (an entity
        // may run a BT and a FSM and a GOAP brain simultaneously).
        void PublishToAIBlackboards(Entity entity, const PerceptionComponent& pc)
        {
            if (entity.HasComponent<BehaviorTreeComponent>())
            {
                WriteToBlackboard(entity.GetComponent<BehaviorTreeComponent>().Blackboard, pc);
            }
            if (entity.HasComponent<StateMachineComponent>())
            {
                WriteToBlackboard(entity.GetComponent<StateMachineComponent>().Blackboard, pc);
            }
            if (entity.HasComponent<GoapAgentComponent>())
            {
                WriteToBlackboard(entity.GetComponent<GoapAgentComponent>().Blackboard, pc);
            }
        }

        // True if a solid body sits between the eye and the target. With no live
        // physics scene we cannot test occlusion, so the line is treated as clear
        // (range + FOV already passed) — sight degrades to "see-through" rather
        // than blind, which is the useful default for headless / editor-stopped
        // contexts.
        bool IsOccluded(const Scene* scene, const glm::vec3& eye, const glm::vec3& targetPos,
                        UUID perceiver, UUID target)
        {
            JoltScene* physics = scene->GetPhysicsScene();
            if ((physics == nullptr) || !physics->IsInitialized())
            {
                return false;
            }

            const glm::vec3 toTarget = targetPos - eye;
            const f32 distance = glm::length(toTarget);
            if (distance <= 0.0001f)
            {
                return false;
            }

            RayCastInfo ray;
            ray.m_Origin = eye;
            ray.m_Direction = toTarget / distance;
            ray.m_MaxDistance = distance;
            // Exclude both endpoints: the perceiver's own body must not block its
            // eyes, and the target's collider is exactly what we are trying to
            // see — only a *third* body in between counts as an occluder.
            ray.m_ExcludedEntities.push_back(perceiver);
            ray.m_ExcludedEntities.push_back(target);

            SceneQueryHit hit;
            return physics->CastRay(ray, hit) && hit.HasHit();
        }
    } // namespace

    void PerceptionSystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        auto perceivers = scene->GetAllEntitiesWith<PerceptionComponent, TransformComponent>();
        auto targets = scene->GetAllEntitiesWith<PerceptibleComponent, TransformComponent>();

        for (auto perceiverId : perceivers)
        {
            Entity perceiver = { perceiverId, scene };
            auto& pc = perceiver.GetComponent<PerceptionComponent>();
            const auto& perceiverTransform = perceiver.GetComponent<TransformComponent>();

            // Eye position and look direction in world space. Forward is the
            // entity's local -Z (engine convention; see EditorCamera / fly-cam).
            const glm::quat orientation = perceiverTransform.GetRotation();
            const glm::vec3 eye = perceiverTransform.Translation + (orientation * pc.EyeOffset);
            const glm::vec3 forward = glm::normalize(orientation * glm::vec3(0.0f, 0.0f, -1.0f));

            const UUID perceiverUUID = perceiver.GetUUID();

            bool found = false;
            UUID bestTarget = 0;
            glm::vec3 bestTargetPos = { 0.0f, 0.0f, 0.0f };
            f32 bestDistSq = std::numeric_limits<f32>::max();

            for (auto targetId : targets)
            {
                if (targetId == perceiverId)
                {
                    continue; // a sensor never senses itself
                }

                Entity targetEntity = { targetId, scene };
                const auto& perceptible = targetEntity.GetComponent<PerceptibleComponent>();
                if (!perceptible.IsPerceptible)
                {
                    continue; // hidden / cloaked
                }
                if (!pc.DetectSameTeam && (perceptible.Team == pc.PerceiverTeam))
                {
                    continue; // ally — filtered out
                }

                const glm::vec3 targetPos = targetEntity.GetComponent<TransformComponent>().Translation;
                const f32 distSq = glm::dot(targetPos - eye, targetPos - eye);
                if (distSq >= bestDistSq)
                {
                    continue; // a nearer target already won — skip costlier checks
                }

                // Range + field-of-view cone gate (single source of truth).
                if (!PerceptionMath::IsInSightCone(eye, forward, targetPos, pc.SightRange, pc.FovDegrees))
                {
                    continue;
                }

                // Optional line-of-sight raycast.
                if (pc.RequireLineOfSight &&
                    IsOccluded(scene, eye, targetPos, perceiverUUID, targetEntity.GetUUID()))
                {
                    continue; // a wall blocks the view
                }

                found = true;
                bestDistSq = distSq;
                bestTarget = targetEntity.GetUUID();
                bestTargetPos = targetPos;
            }

            // Publish the result onto the component...
            pc.HasVisibleTarget = found;
            if (found)
            {
                pc.VisibleTarget = bestTarget;
                pc.LastKnownPosition = bestTargetPos;
                pc.HasLastKnownPosition = true;
                pc.TimeSinceLastSeen = 0.0f;
            }
            else
            {
                pc.VisibleTarget = 0;
                pc.TimeSinceLastSeen += dt;
            }

            // ...and mirror it into the entity's AI blackboard(s).
            PublishToAIBlackboards(perceiver, pc);
        }
    }
} // namespace OloEngine
