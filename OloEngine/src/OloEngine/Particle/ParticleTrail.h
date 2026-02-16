#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // Ring buffer to store trail point history for one particle
    struct TrailPoint
    {
        glm::vec3 Position{ 0.0f };
        f32 Width = 1.0f;
        glm::vec4 Color{ 1.0f };
        f32 Age = 0.0f; // 0 = newest, 1 = oldest
    };

    // Per-particle trail data stored as SOA alongside ParticlePool
    class ParticleTrailData
    {
    public:
        void Resize(u32 maxParticles, u32 maxTrailPoints);

        // Record a new trail point for particle at index
        void RecordPoint(u32 particleIndex, const glm::vec3& position, f32 width, const glm::vec4& color);

        // Swap trail data when pool kills a particle (swap-to-back)
        void SwapParticles(u32 a, u32 b);

        // Clear trail for a particle when it's born
        void ClearTrail(u32 particleIndex);

        // Age all trail points by dt/lifetime
        void AgePoints(f32 dt, f32 trailLifetime);

        // Get trail points for a particle (newest to oldest)
        [[nodiscard]] const std::vector<TrailPoint>& GetTrail(u32 particleIndex) const { return m_Trails[particleIndex]; }
        [[nodiscard]] u32 GetMaxTrailPoints() const { return m_MaxTrailPoints; }

    private:
        std::vector<std::vector<TrailPoint>> m_Trails; // [particle][point]
        u32 m_MaxTrailPoints = 16;
    };

    struct ModuleTrail
    {
        bool Enabled = false;
        u32 MaxTrailPoints = 16;     // Points per particle trail
        f32 TrailLifetime = 0.5f;    // How long trail points last (seconds)
        f32 MinVertexDistance = 0.1f; // Min distance between recorded points
        f32 WidthStart = 1.0f;
        f32 WidthEnd = 0.0f;
        glm::vec4 ColorStart{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 ColorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
    };
}
