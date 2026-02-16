#include "OloEnginePCH.h"
#include "ParticleSystem.h"

namespace OloEngine
{
    ParticleSystem::ParticleSystem(u32 maxParticles)
        : m_Pool(maxParticles)
    {
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);
    }

    void ParticleSystem::SetMaxParticles(u32 maxParticles)
    {
        m_Pool.Resize(maxParticles);
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);
    }

    void ParticleSystem::Update(f32 dt, const glm::vec3& emitterPosition)
    {
        OLO_PROFILE_FUNCTION();

        if (!Playing)
        {
            return;
        }

        f32 scaledDt = dt * PlaybackSpeed;
        m_Time += scaledDt;

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
        Emitter.Update(scaledDt, m_Pool, emitterPosition);
        u32 newAlive = m_Pool.GetAliveCount();

        Emitter.RateOverTime = origRate;

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

        // 2. Apply Phase 1 modules (order matters)
        GravityModule.Apply(scaledDt, m_Pool);
        DragModule.Apply(scaledDt, m_Pool);
        VelocityModule.Apply(scaledDt, m_Pool);
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
                m_TrailData.RecordPoint(i, m_Pool.Positions[i], m_Pool.Sizes[i], m_Pool.Colors[i]);
            }
            m_TrailData.AgePoints(scaledDt, TrailModule.TrailLifetime);
        }

        // 6. Kill expired particles (collect death triggers before killing)
        if (SubEmitterModule.Enabled)
        {
            // Check for dying particles before UpdateLifetimes kills them
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
                            m_PendingTriggers.push_back(trigger);
                        }
                    }
                }
            }
        }

        m_Pool.UpdateLifetimes(scaledDt);
    }

    void ParticleSystem::Reset()
    {
        m_Time = 0.0f;
        m_Pool.Resize(m_Pool.GetMaxParticles());
        m_TrailData.Resize(m_Pool.GetMaxParticles(), TrailModule.MaxTrailPoints);
        m_PendingTriggers.clear();
        Emitter.Reset();
        Playing = true;
    }
}
