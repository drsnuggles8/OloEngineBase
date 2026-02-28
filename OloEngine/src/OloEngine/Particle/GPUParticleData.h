#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // =============================================================================
    // GPU PARTICLE DATA STRUCTURES (std430 layout)
    // =============================================================================

    // Per-particle data stored in the main particle SSBO.
    // Must match the GLSL layout in compute/rendering shaders exactly.
    // Size: 96 bytes (6 × vec4), std430-aligned.
    struct GPUParticle
    {
        glm::vec4 PositionLifetime;    // xyz = world position, w = remaining lifetime
        glm::vec4 VelocityMaxLifetime; // xyz = velocity, w = max lifetime
        glm::vec4 Color;               // rgba
        glm::vec4 InitialColor;        // rgba (at emission time)
        glm::vec4 InitialVelocitySize; // xyz = initial velocity, w = current size
        glm::vec4 Misc;                // x = initial size, y = rotation (radians), z = alive (1.0/0.0), w = entityID as float

        static constexpr u32 GetSize()
        {
            return sizeof(GPUParticle);
        }
    };

    static_assert(sizeof(GPUParticle) == 96, "GPUParticle must be 96 bytes for std430 alignment");

    // Atomic counters and metadata stored in the counter SSBO.
    // Read/written by compute shaders via atomicAdd.
    struct GPUParticleCounters
    {
        u32 AliveCount; // Number of alive particles (written by Compact)
        u32 DeadCount;  // Number of free slots available (written by Compact)
        u32 EmitCount;  // Number of particles to emit this frame (written by CPU)
        u32 Pad;        // Padding for 16-byte alignment
    };

    static_assert(sizeof(GPUParticleCounters) == 16, "GPUParticleCounters must be 16 bytes");

    // DrawElementsIndirectCommand for indirect draw calls.
    // Matches GL_DRAW_INDIRECT_BUFFER layout.
    struct DrawElementsIndirectCommand
    {
        u32 Count;         // Number of indices per instance (6 for a quad)
        u32 InstanceCount; // Number of instances to draw (= alive count)
        u32 FirstIndex;    // Starting index in the index buffer
        u32 BaseVertex;    // Offset added to each index
        u32 BaseInstance;  // First instance ID
    };

    static_assert(sizeof(DrawElementsIndirectCommand) == 20, "DrawElementsIndirectCommand must be 20 bytes");

    // Simulation parameters uploaded as uniforms to the simulation compute shader.
    // CPU fills this each frame from the ParticleSystem settings.
    struct GPUSimParams
    {
        f32 DeltaTime = 0.0f;
        f32 DragCoefficient = 0.0f;
        f32 Pad0 = 0.0f;
        f32 Pad1 = 0.0f;
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };
        u32 MaxParticles = 0;

        // Module enable flags (sent as int uniforms)
        i32 EnableGravity = 0;
        i32 EnableDrag = 0;
        i32 EnableWind = 0;  // Wind field sampling
        i32 EnableNoise = 0; // Noise-based turbulence

        // Wind influence
        f32 WindInfluence = 1.0f; // 0–1 multiplier on sampled wind velocity

        // Noise turbulence (per-particle procedural variation)
        f32 NoiseStrength = 0.0f;  // Amplitude of noise force
        f32 NoiseFrequency = 1.0f; // Spatial frequency of noise
        f32 NoisePad = 0.0f;

        // Collision
        i32 EnableGroundCollision = 0; // Ground plane collision
        f32 GroundY = 0.0f;            // Ground plane height
        f32 CollisionBounce = 0.3f;    // Coefficient of restitution (0–1)
        f32 CollisionFriction = 0.8f;  // Tangential velocity damping (0–1)
    };

} // namespace OloEngine
