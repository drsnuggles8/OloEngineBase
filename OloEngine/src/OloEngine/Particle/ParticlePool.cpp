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
        m_MaxParticles = maxParticles;
        m_AliveCount = 0;

        Positions.resize(maxParticles);
        Velocities.resize(maxParticles);
        Colors.resize(maxParticles);
        Sizes.resize(maxParticles);
        Rotations.resize(maxParticles);
        Lifetimes.resize(maxParticles);
        MaxLifetimes.resize(maxParticles);
        InitialColors.resize(maxParticles);
        InitialSizes.resize(maxParticles);
        InitialVelocities.resize(maxParticles);
    }

    u32 ParticlePool::Emit(u32 count)
    {
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
        u32 i = 0;
        while (i < m_AliveCount)
        {
            Lifetimes[i] -= dt;
            if (Lifetimes[i] <= 0.0f)
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
        if (MaxLifetimes[index] <= 0.0f)
        {
            return 1.0f;
        }
        return 1.0f - (Lifetimes[index] / MaxLifetimes[index]);
    }

    void ParticlePool::SwapParticles(u32 a, u32 b)
    {
        std::swap(Positions[a], Positions[b]);
        std::swap(Velocities[a], Velocities[b]);
        std::swap(Colors[a], Colors[b]);
        std::swap(Sizes[a], Sizes[b]);
        std::swap(Rotations[a], Rotations[b]);
        std::swap(Lifetimes[a], Lifetimes[b]);
        std::swap(MaxLifetimes[a], MaxLifetimes[b]);
        std::swap(InitialColors[a], InitialColors[b]);
        std::swap(InitialSizes[a], InitialSizes[b]);
        std::swap(InitialVelocities[a], InitialVelocities[b]);

        if (OnSwapCallback)
        {
            OnSwapCallback(a, b);
        }
    }
}
