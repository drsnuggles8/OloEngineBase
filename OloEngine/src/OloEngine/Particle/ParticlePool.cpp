#include "OloEnginePCH.h"
#include "ParticlePool.h"

namespace OloEngine
{
    ParticlePool::ParticlePool(u32 maxParticles)
    {
        Resize(maxParticles);
    }

    void ParticlePool::Resize(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        m_MaxParticles = maxParticles;
        m_AliveCount = 0;

        m_Positions.resize(maxParticles);
        m_Velocities.resize(maxParticles);
        m_Colors.resize(maxParticles);
        m_Sizes.resize(maxParticles);
        m_Rotations.resize(maxParticles);
        m_Lifetimes.resize(maxParticles);
        m_MaxLifetimes.resize(maxParticles);
        m_InitialColors.resize(maxParticles);
        m_InitialSizes.resize(maxParticles);
        m_InitialVelocities.resize(maxParticles);
    }

    u32 ParticlePool::Emit(u32 count)
    {
        OLO_PROFILE_FUNCTION();

        u32 available = m_MaxParticles - m_AliveCount;
        u32 toEmit = std::min(count, available);

        // Newly emitted particles occupy slots [m_AliveCount .. m_AliveCount + toEmit)
        // Caller is responsible for initializing these slots
        m_AliveCount += toEmit;
        return toEmit;
    }

    void ParticlePool::Kill(u32 index)
    {
        if (index >= m_AliveCount)
        {
            return;
        }

        u32 last = m_AliveCount - 1;
        if (index != last)
        {
            SwapParticles(index, last);
        }
        --m_AliveCount;
    }

    void ParticlePool::UpdateLifetimes(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        u32 i = 0;
        while (i < m_AliveCount)
        {
            m_Lifetimes[i] -= dt;
            if (m_Lifetimes[i] <= 0.0f)
            {
                Kill(i);
                // Don't increment — the swapped-in particle now occupies index i
            }
            else
            {
                ++i;
            }
        }
    }

    f32 ParticlePool::GetAge(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_AliveCount, "ParticlePool::GetAge index out of range!");
        if (m_MaxLifetimes[index] <= 0.0f)
        {
            return 1.0f;
        }
        return 1.0f - (m_Lifetimes[index] / m_MaxLifetimes[index]);
    }

    void ParticlePool::SwapParticles(u32 a, u32 b)
    {
        OLO_PROFILE_FUNCTION();

        std::swap(m_Positions[a], m_Positions[b]);
        std::swap(m_Velocities[a], m_Velocities[b]);
        std::swap(m_Colors[a], m_Colors[b]);
        std::swap(m_Sizes[a], m_Sizes[b]);
        std::swap(m_Rotations[a], m_Rotations[b]);
        std::swap(m_Lifetimes[a], m_Lifetimes[b]);
        std::swap(m_MaxLifetimes[a], m_MaxLifetimes[b]);
        std::swap(m_InitialColors[a], m_InitialColors[b]);
        std::swap(m_InitialSizes[a], m_InitialSizes[b]);
        std::swap(m_InitialVelocities[a], m_InitialVelocities[b]);

        if (m_OnSwapCallback)
        {
            m_OnSwapCallback(a, b);
        }
    }
} // namespace OloEngine
