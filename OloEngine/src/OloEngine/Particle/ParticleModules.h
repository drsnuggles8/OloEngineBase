#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticleCurve.h"
#include "OloEngine/Particle/ParticlePool.h"

#include <glm/glm.hpp>
#include <variant>

namespace OloEngine
{
    // --- Individual modules ---

    struct ModuleColorOverLifetime
    {
        bool Enabled = false;
        ParticleCurve4 ColorCurve{ glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) };

        void Apply(ParticlePool& pool) const;
    };

    struct ModuleSizeOverLifetime
    {
        bool Enabled = false;
        ParticleCurve SizeCurve{ 1.0f, 0.0f };

        void Apply(ParticlePool& pool) const;
    };

    struct ModuleVelocityOverLifetime
    {
        bool Enabled = false;
        glm::vec3 LinearVelocity{ 0.0f };
        f32 SpeedMultiplier = 1.0f;
        ParticleCurve SpeedCurve{ 1.0f };

        void Apply(f32 dt, ParticlePool& pool) const;
    };

    struct ModuleRotationOverLifetime
    {
        bool Enabled = false;
        f32 AngularVelocity = 0.0f; // degrees per second

        void Apply(f32 dt, ParticlePool& pool) const;
    };

    struct ModuleGravity
    {
        bool Enabled = false;
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };

        void Apply(f32 dt, ParticlePool& pool) const;
    };

    struct ModuleDrag
    {
        bool Enabled = false;
        f32 DragCoefficient = 0.1f;

        void Apply(f32 dt, ParticlePool& pool) const;
    };

    struct ModuleNoise
    {
        bool Enabled = false;
        f32 Strength = 1.0f;
        f32 Frequency = 1.0f;

        void Apply(f32 dt, f32 time, ParticlePool& pool) const;
    };

    enum class TextureSheetAnimMode : u8
    {
        OverLifetime = 0, // Frame index driven by particle age
        BySpeed           // Frame index driven by particle speed
    };

    struct ModuleTextureSheetAnimation
    {
        bool Enabled = false;
        u32 GridX = 1;       // Columns
        u32 GridY = 1;       // Rows
        u32 TotalFrames = 1; // May be less than GridX * GridY
        TextureSheetAnimMode Mode = TextureSheetAnimMode::OverLifetime;
        f32 SpeedRange = 10.0f; // Speed at which last frame is reached (BySpeed mode)

        // Compute the UV min/max for a given frame index
        void GetFrameUV(u32 frame, glm::vec2& uvMin, glm::vec2& uvMax) const;
    };
} // namespace OloEngine
