#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Particle/EmissionShape.h"
#include "OloEngine/Particle/ParticlePool.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    struct BurstEntry
    {
        f32 Time = 0.0f; // Time offset within loop
        u32 Count = 10;
        f32 Probability = 1.0f; // 0..1

        auto operator==(const BurstEntry&) const -> bool = default;
    };

    class ParticleEmitter
    {
      public:
        // Emission settings
        f32 RateOverTime = 10.0f; // Particles per second
        f32 InitialSpeed = { 5.0f };
        f32 SpeedVariance = 0.0f;
        f32 LifetimeMin = 1.0f;
        f32 LifetimeMax = 2.0f;
        f32 InitialSize = 1.0f;
        f32 SizeVariance = 0.0f;
        f32 InitialRotation = 0.0f;
        f32 RotationVariance = 0.0f;
        glm::vec4 InitialColor{ 1.0f, 1.0f, 1.0f, 1.0f };

        EmissionShape Shape;

        TArray<BurstEntry> Bursts;

        // Emit particles for this frame, returns number emitted. Draws all
        // randomness (burst probability, spawn jitter, emission-shape sampling)
        // from the caller-supplied `rng` so emission is deterministic per
        // owning ParticleSystem rather than tied to the thread_local global
        // stream (issue #452 / #576).
        u32 Update(f32 dt, ParticlePool& pool, const glm::vec3& emitterPosition, f32 rateMultiplier, const glm::quat& emitterRotation, FastRandomPCG& rng);

        void Reset();

        // Compares the authored settings above and nothing else. The private
        // emission cursor (accumulator, loop time, next burst) advances every
        // tick, including the editor's Edit-mode preview, so it is deliberately
        // not part of "the same emitter" (#1412).
        [[nodiscard]] bool HasSameSettings(const ParticleEmitter& other) const
        {
            return Math::BitwiseEqual(RateOverTime, other.RateOverTime) &&
                   Math::BitwiseEqual(InitialSpeed, other.InitialSpeed) &&
                   Math::BitwiseEqual(SpeedVariance, other.SpeedVariance) &&
                   Math::BitwiseEqual(LifetimeMin, other.LifetimeMin) &&
                   Math::BitwiseEqual(LifetimeMax, other.LifetimeMax) &&
                   Math::BitwiseEqual(InitialSize, other.InitialSize) &&
                   Math::BitwiseEqual(SizeVariance, other.SizeVariance) &&
                   Math::BitwiseEqual(InitialRotation, other.InitialRotation) &&
                   Math::BitwiseEqual(RotationVariance, other.RotationVariance) &&
                   Math::BitwiseEqual(InitialColor, other.InitialColor) && Shape == other.Shape &&
                   Bursts == other.Bursts;
        }

      private:
        friend struct TIsTriviallyRelocatable<ParticleEmitter>;
        void InitializeParticle(u32 index, ParticlePool& pool, const glm::vec3& emitterPosition, const glm::quat& emitterRotation, FastRandomPCG& rng) const;

        f32 m_EmitAccumulator = 0.0f;
        f32 m_LoopTime = 0.0f;
        u32 m_NextBurstIndex = 0;
    };
    // Owned arrays/resources use independent heap storage; the remaining fields
    // are values or external pointers. No member retains the enclosing address.
    template<>
    struct TIsTriviallyRelocatable<ParticleEmitter>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ParticleEmitter::RateOverTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::InitialSpeed)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::SpeedVariance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::LifetimeMin)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::LifetimeMax)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::InitialSize)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::SizeVariance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::InitialRotation)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::RotationVariance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::InitialColor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::Shape)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::Bursts)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::m_EmitAccumulator)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::m_LoopTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ParticleEmitter::m_NextBurstIndex)>::Value;
    };
} // namespace OloEngine
