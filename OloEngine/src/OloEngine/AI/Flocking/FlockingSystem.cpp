#include "OloEnginePCH.h"

#include "OloEngine/AI/Flocking/FlockingSystem.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Below this the direction of a vector is meaningless and normalizing
        // it would produce a NaN. Squared, so callers compare against dot().
        constexpr f32 kDegenerateLengthSq = 1.0e-12f;

        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
    } // namespace

    void FlockingWorkspace::Clear()
    {
        BoidPositions.clear();
        BoidVelocities.clear();
        BoidGrid.Clear();
        ObstaclePositions.clear();
        ObstacleRadii.clear();
        ObstacleGrid.Clear();
        MaxObstacleRadius = 0.0f;
    }

    glm::vec3 FlockingSystem::ClampLength(const glm::vec3& v, f32 maxLength)
    {
        if (!IsFinite(v) || !(maxLength > 0.0f))
            return glm::vec3(0.0f);

        const f32 lengthSq = glm::dot(v, v);
        if (lengthSq <= kDegenerateLengthSq)
            return glm::vec3(0.0f);
        if (lengthSq <= maxLength * maxLength)
            return v;

        return v * (maxLength / std::sqrt(lengthSq));
    }

    glm::vec3 FlockingSystem::SteerTowards(const glm::vec3& desiredDirection, const glm::vec3& velocity, f32 maxSpeed,
                                           f32 maxForce)
    {
        if (!IsFinite(desiredDirection) || !IsFinite(velocity))
            return glm::vec3(0.0f);

        const f32 lengthSq = glm::dot(desiredDirection, desiredDirection);
        if (lengthSq <= kDegenerateLengthSq)
            return glm::vec3(0.0f);

        const glm::vec3 desiredVelocity = desiredDirection * (maxSpeed / std::sqrt(lengthSq));
        return ClampLength(desiredVelocity - velocity, maxForce);
    }

    void FlockingSystem::StepSteering(Scene* scene, FlockingWorkspace& workspace)
    {
        OLO_PROFILE_FUNCTION();

        if (scene == nullptr)
            return;

        // Plain views, not owning groups, on purpose: an owning group reorders
        // the owned pool, and BoidComponent has no other hot loop to justify
        // claiming that ownership (see the ownership map at the top of
        // Scene.cpp). The per-agent cost here is dominated by the neighbour
        // query, not by the view walk.
        auto boidView = scene->GetAllEntitiesWith<TransformComponent, BoidComponent>();

        workspace.BoidPositions.clear();
        workspace.BoidVelocities.clear();

        // ── Pass 1: freeze the flock ─────────────────────────────────────────
        f32 maxNeighborRadius = FlockSpatialHash::kMinCellSize;
        f32 maxLookahead = 0.0f;
        for (const auto entity : boidView)
        {
            const auto& transform = boidView.get<TransformComponent>(entity);
            const auto& boid = boidView.get<BoidComponent>(entity);

            workspace.BoidPositions.push_back(transform.Translation);
            workspace.BoidVelocities.push_back(boid.m_Velocity);

            maxNeighborRadius = std::max(maxNeighborRadius, std::max(boid.m_NeighborRadius, boid.m_SeparationRadius));
            maxLookahead = std::max(maxLookahead, boid.m_ObstacleLookahead);
        }

        if (workspace.BoidPositions.empty())
        {
            workspace.Clear();
            return;
        }

        // Cell size == the widest perception radius in the flock, so a query
        // never sweeps more than 3x3x3 cells.
        workspace.BoidGrid.Rebuild(workspace.BoidPositions, maxNeighborRadius);

        // ── Obstacles ────────────────────────────────────────────────────────
        workspace.ObstaclePositions.clear();
        workspace.ObstacleRadii.clear();
        workspace.MaxObstacleRadius = 0.0f;
        for (auto obstacleView = scene->GetAllEntitiesWith<TransformComponent, BoidObstacleComponent>();
             const auto entity : obstacleView)
        {
            const auto& transform = obstacleView.get<TransformComponent>(entity);
            const auto& obstacle = obstacleView.get<BoidObstacleComponent>(entity);

            workspace.ObstaclePositions.push_back(transform.Translation);
            workspace.ObstacleRadii.push_back(obstacle.m_Radius);
            workspace.MaxObstacleRadius = std::max(workspace.MaxObstacleRadius, obstacle.m_Radius);
        }

        if (!workspace.ObstaclePositions.empty())
        {
            // An obstacle is a sphere, so an agent's query radius is its
            // lookahead plus the largest obstacle radius; sizing the cells to
            // that keeps the obstacle sweep at 27 cells too.
            const f32 obstacleCellSize = std::max(maxLookahead + workspace.MaxObstacleRadius,
                                                  FlockSpatialHash::kMinCellSize);
            workspace.ObstacleGrid.Rebuild(workspace.ObstaclePositions, obstacleCellSize);
        }
        else
        {
            workspace.ObstacleGrid.Clear();
        }

        // ── Pass 2: solve each agent's steering force ────────────────────────
        // Same view, same tick, no structural change in between, so index `i`
        // lines up with the snapshot taken in pass 1.
        u32 i = 0;
        for (const auto entity : boidView)
        {
            auto& boid = boidView.get<BoidComponent>(entity);
            const glm::vec3 position = workspace.BoidPositions[i];
            const glm::vec3 velocity = workspace.BoidVelocities[i];
            const u32 selfIndex = i;
            ++i;

            boid.m_SteeringForce = glm::vec3(0.0f);
            boid.m_NeighborCount = 0;

            if (!IsFinite(position) || !IsFinite(velocity))
                continue;

            glm::vec3 separation(0.0f);
            glm::vec3 alignment(0.0f);
            glm::vec3 cohesion(0.0f);
            u32 neighborCount = 0;
            u32 separationCount = 0;

            const f32 separationRadiusSq = boid.m_SeparationRadius * boid.m_SeparationRadius;
            const u32 maxNeighbors = std::max(1u, boid.m_MaxNeighbors);

            workspace.BoidGrid.ForEachInRadius(
                position, boid.m_NeighborRadius,
                [&](u32 index, const glm::vec3& neighborPosition, f32 distanceSq) -> bool
                {
                    if (index == selfIndex)
                        return true;

                    ++neighborCount;
                    cohesion += neighborPosition;
                    alignment += workspace.BoidVelocities[index];

                    if (distanceSq < separationRadiusSq && distanceSq > kDegenerateLengthSq)
                    {
                        // Weight repulsion by 1/d so a near-collision dominates
                        // a merely-close neighbour.
                        separation += (position - neighborPosition) / distanceSq;
                        ++separationCount;
                    }

                    // Stop once the cap is reached — the visit order is
                    // deterministic, so "the first N" is a reproducible set.
                    return neighborCount < maxNeighbors;
                });

            glm::vec3 force(0.0f);
            const f32 maxSpeed = boid.m_MaxSpeed;
            const f32 maxForce = boid.m_MaxForce;

            if (neighborCount > 0)
            {
                const f32 inverseCount = 1.0f / static_cast<f32>(neighborCount);

                // Cohesion: steer towards the neighbourhood's centre of mass.
                const glm::vec3 centre = cohesion * inverseCount;
                force += boid.m_CohesionWeight * SteerTowards(centre - position, velocity, maxSpeed, maxForce);

                // Alignment: match the neighbourhood's average heading.
                force += boid.m_AlignmentWeight * SteerTowards(alignment * inverseCount, velocity, maxSpeed, maxForce);
            }

            if (separationCount > 0)
            {
                force += boid.m_SeparationWeight * SteerTowards(separation, velocity, maxSpeed, maxForce);
            }

            if (boid.m_GoalWeight > 0.0f && IsFinite(boid.m_GoalPosition))
            {
                force += boid.m_GoalWeight * SteerTowards(boid.m_GoalPosition - position, velocity, maxSpeed, maxForce);
            }

            if (boid.m_ObstacleAvoidWeight > 0.0f && workspace.ObstacleGrid.GetIndexedItemCount() > 0)
            {
                const f32 queryRadius = boid.m_ObstacleLookahead + workspace.MaxObstacleRadius;
                glm::vec3 avoidance(0.0f);
                // The urgency ramp has to be applied to the STEERING WEIGHT, not
                // just folded into the direction: SteerTowards renormalizes its
                // input, so with a single obstacle in range a magnitude baked
                // into `avoidance` is discarded entirely and avoidance becomes
                // all-or-nothing — full-strength repulsion the instant an agent
                // crosses the reach boundary. That reads as a flock stalling in
                // front of an obstacle instead of flowing around it, and the
                // shape of the ramp makes no difference at all. Keeping the
                // closest obstacle's urgency and scaling the whole term by it
                // restores the intended soft approach.
                f32 maxUrgency = 0.0f;
                workspace.ObstacleGrid.ForEachInRadius(
                    position, queryRadius,
                    [&](u32 index, const glm::vec3& obstaclePosition, f32 distanceSq)
                    {
                        const f32 reach = workspace.ObstacleRadii[index] + boid.m_ObstacleLookahead;
                        if (distanceSq >= reach * reach || distanceSq <= kDegenerateLengthSq)
                            return;

                        // Ramp from 0 at the edge of the reach to 1 at the
                        // obstacle's centre, so an agent skimming past is
                        // nudged and one heading into the sphere is shoved.
                        // Per-obstacle here so the summed DIRECTION still leans
                        // towards escaping the nearest one.
                        const f32 distance = std::sqrt(distanceSq);
                        const f32 urgency = 1.0f - (distance / reach);
                        avoidance += ((position - obstaclePosition) / distance) * urgency;
                        maxUrgency = std::max(maxUrgency, urgency);
                    });

                if (maxUrgency > 0.0f && glm::dot(avoidance, avoidance) > kDegenerateLengthSq)
                {
                    force += (boid.m_ObstacleAvoidWeight * maxUrgency) *
                             SteerTowards(avoidance, velocity, maxSpeed, maxForce);
                }
            }

            if (boid.m_LockYAxis)
                force.y = 0.0f;

            boid.m_SteeringForce = ClampLength(force, maxForce);
            boid.m_NeighborCount = neighborCount;
        }
    }

    void FlockingSystem::ApplyMovement(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (scene == nullptr || !std::isfinite(dt) || dt <= 0.0f)
            return;

        for (auto view = scene->GetAllEntitiesWith<TransformComponent, BoidComponent>(); const auto entity : view)
        {
            auto& transform = view.get<TransformComponent>(entity);
            auto& boid = view.get<BoidComponent>(entity);

            if (!IsFinite(boid.m_Velocity))
                boid.m_Velocity = glm::vec3(0.0f);
            if (!IsFinite(boid.m_SteeringForce))
                boid.m_SteeringForce = glm::vec3(0.0f);

            // Semi-implicit Euler at the scheduler's FIXED timestep: velocity
            // first, then position from the new velocity. Frame pacing changes
            // how many ticks run, never what one tick does (issue #452).
            glm::vec3 velocity = boid.m_Velocity + boid.m_SteeringForce * dt;
            if (boid.m_LockYAxis)
                velocity.y = 0.0f;
            velocity = ClampLength(velocity, boid.m_MaxSpeed);

            boid.m_Velocity = velocity;

            if (!IsFinite(transform.Translation))
                continue;

            transform.Translation += velocity * dt;

            if (boid.m_FaceVelocity && glm::dot(velocity, velocity) > kDegenerateLengthSq)
            {
                // Engine convention is -Z forward (see PerceptionSystem's sight
                // cone), which is what glm::quatLookAt builds.
                const glm::vec3 forward = glm::normalize(velocity);
                constexpr glm::vec3 kWorldUp(0.0f, 1.0f, 0.0f);
                // Degenerate when travelling straight up/down — keep the
                // previous orientation rather than emitting a NaN quaternion.
                if (std::abs(glm::dot(forward, kWorldUp)) < 0.999f)
                {
                    transform.SetRotation(glm::quatLookAt(forward, kWorldUp));
                }
            }
        }
    }
} // namespace OloEngine
