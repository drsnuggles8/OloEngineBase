#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // Forward declaration — collision module can optionally use Jolt for scene raycasts
    class JoltScene;

    // Collision event data for external consumption (sub-emitters, etc.)
    struct CollisionEvent
    {
        glm::vec3 Position;
        glm::vec3 Velocity;
    };

    enum class CollisionMode : u8
    {
        WorldPlane = 0, // Simple infinite plane collision (fastest)
        SceneRaycast,   // Jolt physics scene raycasts (expensive, per-particle)
    };

    struct ModuleCollision
    {
        bool Enabled = false;
        CollisionMode Mode = CollisionMode::WorldPlane;

        // Plane collision settings
        glm::vec3 PlaneNormal{ 0.0f, 1.0f, 0.0f };
        f32 PlaneOffset = 0.0f; // distance from origin along normal

        // Bounce/kill settings
        f32 Bounce = 0.5f;        // Velocity multiplier on bounce (0 = no bounce, 1 = perfect elastic)
        f32 LifetimeLoss = 0.0f;  // Fraction of remaining lifetime lost on collision
        bool KillOnCollide = false;

        // Apply collision response to all alive particles
        void Apply(f32 dt, ParticlePool& pool, std::vector<CollisionEvent>* outEvents = nullptr) const;

        // Apply with Jolt scene raycasts (more expensive)
        void ApplyWithRaycasts(f32 dt, ParticlePool& pool, JoltScene* joltScene, std::vector<CollisionEvent>* outEvents = nullptr) const;
    };

    enum class ForceFieldType : u8
    {
        Attraction = 0, // Pull toward center
        Repulsion,      // Push away from center
        Vortex,         // Spin around axis
    };

    struct ModuleForceField
    {
        bool Enabled = false;
        ForceFieldType Type = ForceFieldType::Attraction;
        glm::vec3 Position{ 0.0f };      // World-space center of force field
        f32 Strength = 10.0f;
        f32 Radius = 10.0f;              // Falloff radius (0 = infinite range)
        glm::vec3 Axis{ 0.0f, 1.0f, 0.0f }; // For vortex: spin axis

        void Apply(f32 dt, ParticlePool& pool) const;
    };
}
