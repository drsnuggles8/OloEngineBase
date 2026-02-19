#include "OloEnginePCH.h"
#include "ParticleEmitter.h"

namespace OloEngine
{
    u32 ParticleEmitter::Update(f32 dt, ParticlePool& pool, const glm::vec3& emitterPosition, f32 rateMultiplier, const glm::quat& emitterRotation)
    {
        OLO_PROFILE_FUNCTION();

        u32 totalEmitted = 0;
        f32 prevLoopTime = m_LoopTime;
        m_LoopTime += dt;

        // Rate-based emission (apply LOD multiplier without mutating public RateOverTime)
        m_EmitAccumulator += RateOverTime * rateMultiplier * dt;
        u32 rateCount = static_cast<u32>(m_EmitAccumulator);
        m_EmitAccumulator -= static_cast<f32>(rateCount);

        if (rateCount > 0)
        {
            u32 firstSlot = pool.GetAliveCount();
            u32 emitted = pool.Emit(rateCount);
            for (u32 i = 0; i < emitted; ++i)
            {
                InitializeParticle(firstSlot + i, pool, emitterPosition, emitterRotation);
            }
            totalEmitted += emitted;
        }

        // Burst emission
        auto& rng = RandomUtils::GetGlobalRandom();
        for (u32 b = m_NextBurstIndex; b < static_cast<u32>(Bursts.size()); ++b)
        {
            auto& burst = Bursts[b];
            if (burst.Time >= prevLoopTime && burst.Time < m_LoopTime)
            {
                if (rng.GetFloat32InRange(0.0f, 1.0f) <= burst.Probability)
                {
                    u32 firstSlot = pool.GetAliveCount();
                    u32 emitted = pool.Emit(burst.Count);
                    for (u32 i = 0; i < emitted; ++i)
                    {
                        InitializeParticle(firstSlot + i, pool, emitterPosition, emitterRotation);
                    }
                    totalEmitted += emitted;
                }
                m_NextBurstIndex = b + 1;
            }
        }

        return totalEmitted;
    }

    void ParticleEmitter::Reset()
    {
        m_EmitAccumulator = 0.0f;
        m_LoopTime = 0.0f;
        m_NextBurstIndex = 0;
    }

    void ParticleEmitter::InitializeParticle(u32 index, ParticlePool& pool, const glm::vec3& emitterPosition, const glm::quat& emitterRotation)
    {
        auto& rng = RandomUtils::GetGlobalRandom();

        pool.Positions[index] = emitterPosition + emitterRotation * SampleEmissionShape(Shape);

        // Apply entity rotation so emission shapes orient with the entity
        glm::vec3 dir = emitterRotation * SampleEmissionDirection(Shape);
        f32 speed = InitialSpeed + rng.GetFloat32InRange(-SpeedVariance, SpeedVariance);
        glm::vec3 velocity = dir * std::max(speed, 0.0f);
        pool.Velocities[index] = velocity;
        pool.InitialVelocities[index] = velocity;

        pool.Colors[index] = InitialColor;
        pool.InitialColors[index] = InitialColor;

        f32 size = InitialSize + rng.GetFloat32InRange(-SizeVariance, SizeVariance);
        pool.Sizes[index] = size;
        pool.InitialSizes[index] = size;

        pool.Rotations[index] = InitialRotation + rng.GetFloat32InRange(-RotationVariance, RotationVariance);

        f32 lifetime = rng.GetFloat32InRange(LifetimeMin, LifetimeMax);
        pool.Lifetimes[index] = lifetime;
        pool.MaxLifetimes[index] = lifetime;
    }
}
