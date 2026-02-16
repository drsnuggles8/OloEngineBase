#include "OloEnginePCH.h"
#include "ParticleModules.h"
#include "OloEngine/Core/FastRandom.h"

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
            pool.Colors[i] = ColorCurve.Evaluate(age);
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
            pool.Sizes[i] = SizeCurve.Evaluate(age);
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
            pool.Velocities[i] = (pool.Velocities[i] + LinearVelocity * dt) * speedMul;
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

        // Simple pseudo-noise: use sin-based displacement for Phase 1
        // Phase 2 will replace with proper Simplex/Perlin noise
        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            f32 phase = static_cast<f32>(i) * 0.7f + time * Frequency;
            glm::vec3 offset{
                std::sin(phase) * Strength * dt,
                std::cos(phase * 1.3f) * Strength * dt,
                std::sin(phase * 0.8f) * Strength * dt
            };
            pool.Velocities[i] += offset;
        }
    }
}
