#include "OloEnginePCH.h"
#include "ParticleModules.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Particle/SimplexNoise.h"

#include <cmath>

namespace OloEngine
{
    void ModuleColorOverLifetime::Apply(ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            pool.m_Colors[i] = pool.m_InitialColors[i] * ColorCurve.Evaluate(age);
        }
    }

    void ModuleSizeOverLifetime::Apply(ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            pool.m_Sizes[i] = pool.m_InitialSizes[i] * SizeCurve.Evaluate(age);
        }
    }

    void ModuleVelocityOverLifetime::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            f32 speedMul = SpeedMultiplier * SpeedCurve.Evaluate(age);

            // Scale the initial velocity component by the curve while preserving
            // accumulated force contributions (gravity, drag, noise, etc.)
            glm::vec3 forceContribution = pool.m_Velocities[i] - pool.m_InitialVelocities[i];
            pool.m_Velocities[i] = pool.m_InitialVelocities[i] * speedMul + forceContribution + LinearAcceleration * dt;
        }
    }

    void ModuleRotationOverLifetime::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        f32 delta = AngularVelocity * dt;
        for (u32 i = 0; i < count; ++i)
        {
            pool.m_Rotations[i] += delta;
        }
    }

    void ModuleGravity::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        glm::vec3 dv = Gravity * dt;
        for (u32 i = 0; i < count; ++i)
        {
            pool.m_Velocities[i] += dv;
        }
    }

    void ModuleDrag::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        f32 factor = std::exp(-DragCoefficient * dt);
        for (u32 i = 0; i < count; ++i)
        {
            pool.m_Velocities[i] *= factor;
        }
    }

    void ModuleNoise::Apply(f32 dt, f32 time, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        // Spatially-coherent Simplex noise evaluated at particle position
        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3& pos = pool.m_Positions[i];
            glm::vec3 samplePos = pos * Frequency + glm::vec3(time);
            glm::vec3 offset{
                SimplexNoise3D(samplePos.x, samplePos.y, samplePos.z) * Strength * dt,
                SimplexNoise3D(samplePos.x + 31.416f, samplePos.y + 47.853f, samplePos.z + 12.791f) * Strength * dt,
                SimplexNoise3D(samplePos.x + 73.156f, samplePos.y + 89.213f, samplePos.z + 55.627f) * Strength * dt
            };
            pool.m_Velocities[i] += offset;
        }
    }

    void ModuleTextureSheetAnimation::GetFrameUV(u32 frame, glm::vec2& uvMin, glm::vec2& uvMax) const
    {
        OLO_PROFILE_FUNCTION();

        if (GridX == 0 || GridY == 0)
        {
            uvMin = { 0.0f, 0.0f };
            uvMax = { 1.0f, 1.0f };
            return;
        }

        frame = frame % (GridX * GridY);
        u32 col = frame % GridX;
        u32 row = frame / GridX;

        f32 cellW = 1.0f / static_cast<f32>(GridX);
        f32 cellH = 1.0f / static_cast<f32>(GridY);

        uvMin = { col * cellW, row * cellH };
        uvMax = { (col + 1) * cellW, (row + 1) * cellH };
    }
} // namespace OloEngine
