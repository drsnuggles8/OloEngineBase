#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class ParticlePool
    {
      public:
        explicit ParticlePool(u32 maxParticles = 1000);

        // Resize all SOA arrays to `maxParticles`.
        // WARNING: resets m_AliveCount to 0 — all alive particle state is lost.
        void Resize(u32 maxParticles);

        // Emit up to `count` particles. Returns how many were actually emitted (capped by capacity).
        u32 Emit(u32 count);

        // Kill particle at index by swapping with the last alive particle
        void Kill(u32 index);

        // Advance lifetimes by dt and kill expired particles
        void UpdateLifetimes(f32 dt);

        // Get normalized age (0..1) for a particle
        [[nodiscard]] f32 GetAge(u32 index) const;

        [[nodiscard]] u32 GetAliveCount() const
        {
            return m_AliveCount;
        }
        [[nodiscard]] u32 GetMaxParticles() const
        {
            return m_MaxParticles;
        }

        // SOA arrays — public for direct module access (performance critical)
        std::vector<glm::vec3> Positions;
        std::vector<glm::vec3> Velocities;
        std::vector<glm::vec4> Colors;
        std::vector<f32> Sizes;
        std::vector<f32> Rotations;
        std::vector<f32> Lifetimes;    // Remaining lifetime
        std::vector<f32> MaxLifetimes; // Initial lifetime (for age calculation)

        // Initial values stored at emission time — used by OverLifetime modules as base multiplier
        std::vector<glm::vec4> InitialColors;
        std::vector<f32> InitialSizes;
        std::vector<glm::vec3> InitialVelocities;

        // Optional callback invoked when particles are swapped during Kill/UpdateLifetimes
        // Use to keep external SOA data synchronized (e.g., trail data)
        std::function<void(u32, u32)> OnSwapCallback;

      private:
        void SwapParticles(u32 a, u32 b);

        u32 m_MaxParticles = 0;
        u32 m_AliveCount = 0;
    };
} // namespace OloEngine
