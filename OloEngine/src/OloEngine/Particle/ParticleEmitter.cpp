#include "OloEnginePCH.h"
#include "ParticleEmitter.h"

#include <algorithm>

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
        OLO_PROFILE_FUNCTION();

        m_EmitAccumulator = 0.0f;
        m_LoopTime = 0.0f;
        m_NextBurstIndex = 0;
        // Sort bursts by time so the forward-iteration in Update() works correctly
        std::sort(Bursts.begin(), Bursts.end(),
                  [](const BurstEntry& a, const BurstEntry& b)
                  { return a.Time < b.Time; });
    }

    void ParticleEmitter::InitializeParticle(u32 index, ParticlePool& pool, const glm::vec3& emitterPosition, const glm::quat& emitterRotation)
    {
        auto& rng = RandomUtils::GetGlobalRandom();

        // Use combined sampler to ensure mesh shapes pick position+direction from the same triangle
        auto emission = SampleEmissionCombined(Shape);
        pool.m_Positions[index] = emitterPosition + emitterRotation * emission.Position;

        // Apply entity rotation so emission shapes orient with the entity
        glm::vec3 dir = emitterRotation * emission.Direction;
        f32 speed = InitialSpeed + rng.GetFloat32InRange(-SpeedVariance, SpeedVariance);
        glm::vec3 velocity = dir * std::max(speed, 0.0f);
        pool.m_Velocities[index] = velocity;
        pool.m_InitialVelocities[index] = velocity;

        pool.m_Colors[index] = InitialColor;
        pool.m_InitialColors[index] = InitialColor;

        f32 size = std::max(InitialSize + rng.GetFloat32InRange(-SizeVariance, SizeVariance), 0.0f);
        pool.m_Sizes[index] = size;
        pool.m_InitialSizes[index] = size;

        pool.m_Rotations[index] = InitialRotation + rng.GetFloat32InRange(-RotationVariance, RotationVariance);

        f32 lifetime = rng.GetFloat32InRange(std::min(LifetimeMin, LifetimeMax), std::max(LifetimeMin, LifetimeMax));
        pool.m_Lifetimes[index] = lifetime;
        pool.m_MaxLifetimes[index] = lifetime;
    }
} // namespace OloEngine
