#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Borrowed only for the duration of a synchronous pool mutation. The pool
    // never retains an owner address, so moving it cannot leave a stale callback.
    struct ParticleSwapObserver
    {
        void* Context = nullptr;
        void (*Notify)(void*, u32, u32) = nullptr;

        void operator()(u32 a, u32 b) const
        {
            if (Notify)
            {
                Notify(Context, a, b);
            }
        }
    };

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
        void Kill(u32 index, ParticleSwapObserver observer = {});

        // Advance lifetimes by dt and kill expired particles
        void UpdateLifetimes(f32 dt, ParticleSwapObserver observer = {});

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
        TArray<glm::vec3> m_Positions;
        // Previous-frame positions, snapshotted by ParticleSystem right before
        // position integration. Used by renderers to compute per-particle
        // motion vectors (scene FB RT3) so TAA can reproject fast-moving
        // particles instead of falling back to neighborhood clip.
        TArray<glm::vec3> m_PrevPositions;
        TArray<glm::vec3> m_Velocities;
        TArray<glm::vec4> m_Colors;
        TArray<f32> m_Sizes;
        TArray<f32> m_Rotations;
        // Previous-frame rotation and size, snapshotted by ParticleSystem
        // right before rotation/size integration. Enables proper billboard
        // quad basis reconstruction and per-mesh prev-model computation for
        // RT3 velocity reprojection (scaling/rotating particles resolve
        // cleanly under TAA instead of smearing).
        TArray<f32> m_PrevRotations;
        TArray<f32> m_PrevSizes;
        TArray<f32> m_Lifetimes;    // Remaining lifetime
        TArray<f32> m_MaxLifetimes; // Initial lifetime (for age calculation)

        // Initial values stored at emission time — used by OverLifetime modules as base multiplier
        TArray<glm::vec4> m_InitialColors;
        TArray<f32> m_InitialSizes;
        TArray<glm::vec3> m_InitialVelocities;

      private:
        friend struct TIsTriviallyRelocatable<ParticlePool>;
        void SwapParticles(u32 a, u32 b, ParticleSwapObserver observer);

        u32 m_MaxParticles = 0;
        u32 m_AliveCount = 0;
    };
    // Owned arrays/resources use independent heap storage; the remaining fields
    // are values or external pointers. No member retains the enclosing address.
    template<>
    struct TIsTriviallyRelocatable<ParticlePool>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ParticlePool::m_Positions)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_PrevPositions)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_Velocities)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_Colors)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_Sizes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_Rotations)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_PrevRotations)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_PrevSizes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_Lifetimes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_MaxLifetimes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_InitialColors)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_InitialSizes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_InitialVelocities)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_MaxParticles)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticlePool::m_AliveCount)>::Value;
    };
} // namespace OloEngine
