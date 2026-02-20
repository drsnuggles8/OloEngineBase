#include "OloEnginePCH.h"
#include "ParticleCollision.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"

namespace OloEngine
{
    void ModuleCollision::Apply([[maybe_unused]] f32 dt, ParticlePool& pool, std::vector<CollisionEvent>* outEvents) const
    {
        if (!Enabled || Mode != CollisionMode::WorldPlane)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        u32 i = 0;
        while (i < count)
        {
            // Signed distance from particle to plane
            f32 dist = glm::dot(pool.m_Positions[i], PlaneNormal) - PlaneOffset;

            if (dist < 0.0f)
            {
                // Record collision event before potential kill
                if (outEvents)
                {
                    outEvents->push_back({ pool.m_Positions[i], pool.m_Velocities[i] });
                }

                if (KillOnCollide)
                {
                    pool.Kill(i);
                    count = pool.GetAliveCount();
                    continue;
                }

                // Push particle back to plane surface
                pool.m_Positions[i] -= PlaneNormal * dist;

                // Reflect velocity
                f32 velDotN = glm::dot(pool.m_Velocities[i], PlaneNormal);
                if (velDotN < 0.0f)
                {
                    pool.m_Velocities[i] -= PlaneNormal * velDotN * (1.0f + Bounce);
                }

                // Apply lifetime loss
                if (LifetimeLoss > 0.0f)
                {
                    pool.m_Lifetimes[i] -= pool.m_Lifetimes[i] * LifetimeLoss;
                }
            }
            ++i;
        }
    }

    void ModuleCollision::ApplyWithRaycasts(f32 dt, ParticlePool& pool, JoltScene* joltScene, std::vector<CollisionEvent>* outEvents) const
    {
        if (!Enabled || Mode != CollisionMode::SceneRaycast || !joltScene)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        u32 i = 0;
        while (i < count)
        {
            glm::vec3 velocity = pool.m_Velocities[i];
            f32 speed = glm::length(velocity);
            if (speed < 0.001f)
            {
                ++i;
                continue;
            }

            glm::vec3 dir = velocity / speed;
            f32 travelDist = speed * dt;

            RayCastInfo ray;
            ray.m_Origin = pool.m_Positions[i];
            ray.m_Direction = dir;
            ray.m_MaxDistance = travelDist;

            SceneQueryHit hit;
            if (joltScene->CastRay(ray, hit) && hit.HasHit())
            {
                // Record collision event before potential kill
                if (outEvents)
                {
                    outEvents->push_back({ pool.m_Positions[i], pool.m_Velocities[i] });
                }

                if (KillOnCollide)
                {
                    pool.Kill(i);
                    count = pool.GetAliveCount();
                    continue;
                }

                // Move to hit point
                pool.m_Positions[i] = hit.m_Position + hit.m_Normal * 0.01f;

                // Reflect velocity off hit normal
                f32 velDotN = glm::dot(pool.m_Velocities[i], hit.m_Normal);
                if (velDotN < 0.0f)
                {
                    pool.m_Velocities[i] -= hit.m_Normal * velDotN * (1.0f + Bounce);
                }

                if (LifetimeLoss > 0.0f)
                {
                    pool.m_Lifetimes[i] -= pool.m_Lifetimes[i] * LifetimeLoss;
                }
            }
            ++i;
        }
    }

    void ModuleForceField::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            glm::vec3 toCenter = Position - pool.m_Positions[i];
            f32 dist = glm::length(toCenter);
            if (dist < 0.001f)
            {
                continue;
            }

            // Falloff: full strength inside radius, zero outside
            // Radius == 0 means no falloff (infinite range, full strength everywhere)
            f32 falloff = 1.0f;
            if (Radius > 0.0f)
            {
                if (dist > Radius)
                {
                    continue; // Outside force field range
                }
                falloff = 1.0f - (dist / Radius);
            }

            glm::vec3 dirToCenter = toCenter / dist;

            switch (Type)
            {
                case ForceFieldType::Attraction:
                    pool.m_Velocities[i] += dirToCenter * Strength * falloff * dt;
                    break;

                case ForceFieldType::Repulsion:
                    pool.m_Velocities[i] -= dirToCenter * Strength * falloff * dt;
                    break;

                case ForceFieldType::Vortex:
                {
                    // Cross product of axis with direction-to-center gives tangent direction
                    glm::vec3 tangent = glm::cross(Axis, dirToCenter);
                    f32 tangentLen = glm::length(tangent);
                    if (tangentLen > 0.001f)
                    {
                        tangent /= tangentLen;
                        pool.m_Velocities[i] += tangent * Strength * falloff * dt;
                    }
                    break;
                }
                default:
                    OLO_CORE_ASSERT(false, "Unknown ForceFieldType!");
                    break;
            }
        }
    }
} // namespace OloEngine
