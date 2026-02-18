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

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            pool.Colors[i] = pool.InitialColors[i] * ColorCurve.Evaluate(age);
        }
    }

    void ModuleSizeOverLifetime::Apply(ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            pool.Sizes[i] = pool.InitialSizes[i] * SizeCurve.Evaluate(age);
        }
    }

    void ModuleVelocityOverLifetime::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 age = pool.GetAge(i);
            f32 speedMul = SpeedMultiplier * SpeedCurve.Evaluate(age);

            // Reconstruct velocity from initial velocity scaled by curve, plus additive linear drift
            f32 elapsedTime = (pool.MaxLifetimes[i] - pool.Lifetimes[i]);
            pool.Velocities[i] = pool.InitialVelocities[i] * speedMul + LinearVelocity * elapsedTime;
        }
    }

    void ModuleRotationOverLifetime::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        f32 delta = AngularVelocity * dt;
        for (u32 i = 0; i < count; ++i)
        {
            pool.Rotations[i] += delta;
        }
    }

    void ModuleGravity::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        glm::vec3 dv = Gravity * dt;
        for (u32 i = 0; i < count; ++i)
        {
            pool.Velocities[i] += dv;
        }
    }

    void ModuleDrag::Apply(f32 dt, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        f32 factor = std::max(1.0f - DragCoefficient * dt, 0.0f);
        for (u32 i = 0; i < count; ++i)
        {
            pool.Velocities[i] *= factor;
        }
    }

    void ModuleNoise::Apply(f32 dt, f32 time, ParticlePool& pool) const
    {
        if (!Enabled)
        {
            return;
        }

        // Spatially-coherent Simplex noise evaluated at particle position
        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3& pos = pool.Positions[i];
            glm::vec3 samplePos = pos * Frequency + glm::vec3(time);
            glm::vec3 offset{
                SimplexNoise3D(samplePos.x, samplePos.y, samplePos.z) * Strength * dt,
                SimplexNoise3D(samplePos.x + 31.416f, samplePos.y + 47.853f, samplePos.z + 12.791f) * Strength * dt,
                SimplexNoise3D(samplePos.x + 73.156f, samplePos.y + 89.213f, samplePos.z + 55.627f) * Strength * dt
            };
            pool.Velocities[i] += offset;
        }
    }

    void ModuleTextureSheetAnimation::GetFrameUV(u32 frame, glm::vec2& uvMin, glm::vec2& uvMax) const
    {
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
}
