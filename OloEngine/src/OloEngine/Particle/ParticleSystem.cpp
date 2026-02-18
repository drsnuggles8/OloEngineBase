#include "OloEnginePCH.h"
#include "ParticleSystem.h"

#include <algorithm>
#include <numeric>

namespace OloEngine
{
    ParticleSystem::ParticleSystem(u32 maxParticles)
        : m_Pool(maxParticles)
    {
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);

        // Wire up swap callback so trail data stays in sync when particles die
        m_Pool.OnSwapCallback = [this](u32 a, u32 b) { m_TrailData.SwapParticles(a, b); };
    }

    void ParticleSystem::SetMaxParticles(u32 maxParticles)
    {
        m_Pool.Resize(maxParticles);
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);
    }

    void ParticleSystem::UpdateLOD(const glm::vec3& cameraPosition, const glm::vec3& emitterPosition)
    {
        f32 dist = glm::length(cameraPosition - emitterPosition);
        if (dist >= LODMaxDistance)
        {
            m_LODSpawnRateMultiplier = 0.0f;
        }
        else if (dist >= LODDistance2)
        {
            m_LODSpawnRateMultiplier = 0.25f;
        }
        else if (dist >= LODDistance1)
        {
            m_LODSpawnRateMultiplier = 0.5f;
        }
        else
        {
            m_LODSpawnRateMultiplier = 1.0f;
        }
    }

    void ParticleSystem::Update(f32 dt, const glm::vec3& emitterPosition, const glm::vec3& parentVelocity)
    {
        OLO_PROFILE_FUNCTION();

        if (!Playing)
        {
            return;
        }

        // Warm-up: pre-simulate on first update to avoid empty systems
        if (!m_HasWarmedUp && WarmUpTime > 0.0f)
        {
            m_HasWarmedUp = true;
            constexpr f32 warmUpStep = 1.0f / 60.0f;
            f32 remaining = WarmUpTime;
            while (remaining > 0.0f)
            {
                f32 step = std::min(remaining, warmUpStep);
                Update(step, emitterPosition, parentVelocity);
                remaining -= step;
            }
            return; // The recursive calls already advanced the system
        }
        m_HasWarmedUp = true;

        f32 scaledDt = dt * PlaybackSpeed;
        m_Time += scaledDt;
        m_EmitterPosition = emitterPosition;
        m_ParentVelocity = parentVelocity;

        // Determine emission position based on simulation space
        glm::vec3 emitPos = (SimulationSpace == ParticleSpace::Local) ? glm::vec3(0.0f) : emitterPosition;

        // Check duration
        if (!Looping && m_Time >= Duration)
        {
            Playing = false;
            return;
        }

        if (Looping && m_Time >= Duration)
        {
            m_Time -= Duration;
            Emitter.Reset();
        }

        // Clear pending sub-emitter triggers from previous frame
        m_PendingTriggers.clear();

        // 1. Emit new particles (with LOD rate multiplier)
        f32 origRate = Emitter.RateOverTime;
        Emitter.RateOverTime *= m_LODSpawnRateMultiplier;

        u32 prevAlive = m_Pool.GetAliveCount();
        Emitter.Update(scaledDt, m_Pool, emitPos);
        u32 newAlive = m_Pool.GetAliveCount();

        Emitter.RateOverTime = origRate;

        // Apply velocity inheritance: add parent entity velocity to newly spawned particles
        if (VelocityInheritance != 0.0f && newAlive > prevAlive)
        {
            glm::vec3 inherited = m_ParentVelocity * VelocityInheritance;
            for (u32 i = prevAlive; i < newAlive; ++i)
            {
                m_Pool.Velocities[i] += inherited;
                m_Pool.InitialVelocities[i] += inherited;
            }
        }

        // Fire OnBirth sub-emitter triggers for newly spawned particles
        if (SubEmitterModule.Enabled && newAlive > prevAlive)
        {
            for (const auto& entry : SubEmitterModule.Entries)
            {
                if (entry.Trigger == SubEmitterEvent::OnBirth)
                {
                    for (u32 i = prevAlive; i < newAlive; ++i)
                    {
                        SubEmitterTriggerInfo trigger;
                        trigger.Position = m_Pool.Positions[i];
                        trigger.Velocity = entry.InheritVelocity ? m_Pool.Velocities[i] * entry.InheritVelocityScale : glm::vec3(0.0f);
                        trigger.Event = SubEmitterEvent::OnBirth;
                        trigger.ChildSystemIndex = entry.ChildSystemIndex;
                        trigger.EmitCount = entry.EmitCount;
                        m_PendingTriggers.push_back(trigger);
                    }
                }
            }
        }

        // Initialize trails for newly spawned particles
        if (TrailModule.Enabled)
        {
            for (u32 i = prevAlive; i < newAlive; ++i)
            {
                m_TrailData.ClearTrail(i);
            }
        }

        // 2. Apply Phase 1 modules (order matters: base velocity first, then forces)
        VelocityModule.Apply(scaledDt, m_Pool);
        GravityModule.Apply(scaledDt, m_Pool);
        DragModule.Apply(scaledDt, m_Pool);
        NoiseModule.Apply(scaledDt, m_Time, m_Pool);
        RotationModule.Apply(scaledDt, m_Pool);
        ColorModule.Apply(m_Pool);
        SizeModule.Apply(m_Pool);

        // 3. Apply Phase 2 modules
        ForceFieldModule.Apply(scaledDt, m_Pool);

        // Collision: use raycasts if Jolt scene available and mode is SceneRaycast
        if (CollisionModule.Enabled)
        {
            if (CollisionModule.Mode == CollisionMode::SceneRaycast && m_JoltScene)
            {
                CollisionModule.ApplyWithRaycasts(scaledDt, m_Pool, m_JoltScene);
            }
            else
            {
                CollisionModule.Apply(scaledDt, m_Pool);
            }
        }

        // 4. Integrate positions
        u32 count = m_Pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            m_Pool.Positions[i] += m_Pool.Velocities[i] * scaledDt;
        }

        // 5. Record trail points after position integration
        if (TrailModule.Enabled)
        {
            count = m_Pool.GetAliveCount();
            for (u32 i = 0; i < count; ++i)
            {
                m_TrailData.RecordPoint(i, m_Pool.Positions[i], m_Pool.Sizes[i], m_Pool.Colors[i], TrailModule.MinVertexDistance);
            }
            m_TrailData.AgePoints(scaledDt, TrailModule.TrailLifetime);
        }

        // 6. Collect death triggers before killing expired particles
        if (SubEmitterModule.Enabled)
        {
            count = m_Pool.GetAliveCount();
            for (u32 i = 0; i < count; ++i)
            {
                if (m_Pool.Lifetimes[i] - scaledDt <= 0.0f)
                {
                    for (const auto& entry : SubEmitterModule.Entries)
                    {
                        if (entry.Trigger == SubEmitterEvent::OnDeath)
                        {
                            SubEmitterTriggerInfo trigger;
                            trigger.Position = m_Pool.Positions[i];
                            trigger.Velocity = entry.InheritVelocity ? m_Pool.Velocities[i] * entry.InheritVelocityScale : glm::vec3(0.0f);
                            trigger.Event = SubEmitterEvent::OnDeath;
                            trigger.ChildSystemIndex = entry.ChildSystemIndex;
                            trigger.EmitCount = entry.EmitCount;
                            m_PendingTriggers.push_back(trigger);
                        }
                    }
                }
            }
        }

        // Kill expired particles (OnSwapCallback keeps trail data in sync)
        m_Pool.UpdateLifetimes(scaledDt);

        // 7. Spawn particles from sub-emitter triggers
        ProcessSubEmitterTriggers();
    }

    void ParticleSystem::ProcessSubEmitterTriggers()
    {
        if (!SubEmitterModule.Enabled || m_PendingTriggers.empty())
        {
            return;
        }

        // Triggers with ChildSystemIndex >= 0 are handled by Scene (emitted into child systems).
        // Only triggers with ChildSystemIndex == -1 fall back to the legacy parent-pool behavior.
        auto& rng = RandomUtils::GetGlobalRandom();

        for (const auto& trigger : m_PendingTriggers)
        {
            // Skip triggers destined for child systems — Scene will process them
            if (trigger.ChildSystemIndex >= 0)
            {
                continue;
            }

            u32 firstSlot = m_Pool.GetAliveCount();
            u32 emitted = m_Pool.Emit(trigger.EmitCount);

            for (u32 i = 0; i < emitted; ++i)
            {
                u32 idx = firstSlot + i;
                m_Pool.Positions[idx] = trigger.Position;

                // Random direction + inherited velocity
                glm::vec3 dir = glm::normalize(glm::vec3(
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f)
                ));
                f32 speed = Emitter.InitialSpeed + rng.GetFloat32InRange(-Emitter.SpeedVariance, Emitter.SpeedVariance);
                glm::vec3 velocity = dir * std::max(speed, 0.0f) + trigger.Velocity;
                m_Pool.Velocities[idx] = velocity;
                m_Pool.InitialVelocities[idx] = velocity;

                m_Pool.Colors[idx] = Emitter.InitialColor;
                m_Pool.InitialColors[idx] = Emitter.InitialColor;

                f32 size = Emitter.InitialSize + rng.GetFloat32InRange(-Emitter.SizeVariance, Emitter.SizeVariance);
                m_Pool.Sizes[idx] = size;
                m_Pool.InitialSizes[idx] = size;
                m_Pool.Rotations[idx] = Emitter.InitialRotation + rng.GetFloat32InRange(-Emitter.RotationVariance, Emitter.RotationVariance);

                f32 lifetime = rng.GetFloat32InRange(Emitter.LifetimeMin, Emitter.LifetimeMax);
                m_Pool.Lifetimes[idx] = lifetime;
                m_Pool.MaxLifetimes[idx] = lifetime;

                if (TrailModule.Enabled)
                {
                    m_TrailData.ClearTrail(idx);
                }
            }
        }
    }

    void ParticleSystem::SortByDepth(const glm::vec3& cameraPosition)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = m_Pool.GetAliveCount();
        m_SortedIndices.resize(count);
        std::iota(m_SortedIndices.begin(), m_SortedIndices.end(), 0u);

        // Sort back-to-front (farthest first) for correct alpha blending
        const auto& positions = m_Pool.Positions;
        std::sort(m_SortedIndices.begin(), m_SortedIndices.end(),
            [&positions, &cameraPosition](u32 a, u32 b)
            {
                f32 distA = glm::dot(positions[a] - cameraPosition, positions[a] - cameraPosition);
                f32 distB = glm::dot(positions[b] - cameraPosition, positions[b] - cameraPosition);
                return distA > distB; // Back-to-front
            });
    }

    void ParticleSystem::Reset()
    {
        m_Time = 0.0f;
        m_HasWarmedUp = false;
        m_Pool.Resize(m_Pool.GetMaxParticles());
        m_TrailData.Resize(m_Pool.GetMaxParticles(), TrailModule.MaxTrailPoints);
        m_PendingTriggers.clear();
        Emitter.Reset();
        Playing = true;
    }
}
